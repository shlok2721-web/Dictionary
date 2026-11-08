#define _GNU_SOURCE
#include "espeak_lib.h"

#include <dlfcn.h>
#include <glob.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#include "utils.h"

/* ---------------------------------------------------------------------- */
/* Extra places to look for libespeak-ng, tried after the system library. */
/* "~/" is replaced by the home directory; shell wildcards are allowed.   */
/* Add more entries here for other setups.                                */
/* ---------------------------------------------------------------------- */
static const char *const EXTRA_LIB_GLOBS[] = {
  "~/myenv/lib/python3*/site-packages/espeakng_loader/libespeak-ng.so*",
  NULL
};

/* ---- the tiny part of the espeak-ng C API we need (no header required) -- */

enum {
  AUDIO_OUTPUT_SYNCHRONOUS = 2,      /* samples go to our callback, no sound */
  INIT_DONT_EXIT           = 0x8000, /* return an error instead of exit()    */
  CHARS_UTF8               = 1,
  FLAG_PHONEMES            = 0x100,  /* honour [[phoneme]] markup (CLI sets it) */
  FLAG_ENDPAUSE            = 0x1000, /* pause at the end, like the CLI           */
  POS_CHARACTER            = 1,
  PARAM_RATE               = 1,
  PARAM_PITCH              = 3,
  PHONEMES_IPA             = 0x02,
  PHONEMES_TIE             = 0x80,
};

typedef int (*SynthCallback) (short *wav, int numsamples, void *events);

/* espeak_VOICE, used to select a voice by language when the name isn't a file. */
typedef struct {
  const char   *name;
  const char   *languages;
  const char   *identifier;
  unsigned char gender, age, variant, xx1;
  int           score;
  void         *spare;
} EspeakVoice;

static struct {
  int         (*Initialize)      (int output, int buflength, const char *path, int options);
  int         (*SetVoiceByName)  (const char *name);
  int         (*SetVoiceByProperties) (EspeakVoice *voice);
  const char *(*TextToPhonemes)  (const void **textptr, int textmode, int phonememode);
  int         (*Synth)           (const void *text, size_t size, unsigned int position,
                                  int position_type, unsigned int end_position,
                                  unsigned int flags, unsigned int *unique_id, void *user_data);
  void        (*SetSynthCallback)(SynthCallback cb);
  int         (*SetParameter)    (int parameter, int value, int relative);
  int         (*Synchronize)     (void);
} api;

static GMutex   lock;
static gboolean tried;          /* load attempted (success or not) */
static gboolean loaded;
static char    *lib_path;       /* never freed once set */
static int      sample_rate;
static char    *current_voice;
static GByteArray *pcm;         /* filled by synth_cb while `lock` is held */

static int
synth_cb (short *wav, int numsamples, void *events)
{
  (void) events;
  if (wav != NULL && numsamples > 0 && pcm != NULL)
    g_byte_array_append (pcm, (const guint8 *) wav, (guint) numsamples * sizeof (short));
  return 0;   /* continue */
}

static gboolean
bind_symbols (void *h)
{
#define SYM(field, name) \
  do { *(void **) &api.field = dlsym (h, name); if (api.field == NULL) return FALSE; } while (0)
  SYM (Initialize,       "espeak_Initialize");
  SYM (SetVoiceByName,   "espeak_SetVoiceByName");
  SYM (SetVoiceByProperties, "espeak_SetVoiceByProperties");
  SYM (TextToPhonemes,   "espeak_TextToPhonemes");
  SYM (Synth,            "espeak_Synth");
  SYM (SetSynthCallback, "espeak_SetSynthCallback");
  SYM (SetParameter,     "espeak_SetParameter");
  SYM (Synchronize,      "espeak_Synchronize");
#undef SYM
  return TRUE;
}

/* Try one library.  FILE_PATH may be a full path or a bare soname. */
static gboolean
try_library (const char *file)
{
  void *h = dlopen (file, RTLD_NOW | RTLD_LOCAL);
  if (h == NULL) {
    log_debug ("espeak library %s: %s", file, dlerror ());
    return FALSE;
  }
  if (!bind_symbols (h)) {
    log_debug ("espeak library %s lacks the expected API", file);
    dlclose (h);
    return FALSE;
  }

  /* Data directory: pip's espeakng_loader ships "espeak-ng-data" right next
   * to the .so.  For the system library, let espeak use its built-in path. */
  g_autofree char *data_parent = NULL;
  if (strchr (file, '/') != NULL) {
    g_autofree char *dir = g_path_get_dirname (file);
    g_autofree char *data = g_build_filename (dir, "espeak-ng-data", NULL);
    if (g_file_test (data, G_FILE_TEST_IS_DIR))
      data_parent = g_steal_pointer (&dir);
  }

  int rate = api.Initialize (AUDIO_OUTPUT_SYNCHRONOUS, 0, data_parent, INIT_DONT_EXIT);
  if (rate <= 0) {
    log_debug ("espeak library %s failed to initialise (data: %s)", file,
               data_parent ? data_parent : "default");
    dlclose (h);
    return FALSE;
  }

  api.SetSynthCallback (synth_cb);
  sample_rate = rate;
  lib_path = g_strdup (file);
  log_info ("Using libespeak-ng fallback: %s (%d Hz)", file, rate);
  return TRUE;
}

static gboolean
try_extra_globs (void)
{
  for (const char *const *g = EXTRA_LIB_GLOBS; *g != NULL; g++) {
    g_autofree char *pattern = g_str_has_prefix (*g, "~/")
      ? g_build_filename (g_get_home_dir (), *g + 2, NULL)
      : g_strdup (*g);
    glob_t gl = { 0 };
    if (glob (pattern, 0, NULL, &gl) == 0) {
      for (size_t i = 0; i < gl.gl_pathc; i++)
        if (try_library (gl.gl_pathv[i])) {
          globfree (&gl);
          return TRUE;
        }
    }
    globfree (&gl);
  }
  return FALSE;
}

/* Caller holds `lock`. */
static gboolean
ensure_loaded (void)
{
  if (tried)
    return loaded;
  tried = TRUE;

  const char *env = g_getenv ("WB_ESPEAK_LIBRARY");
  if (env == NULL || *env == '\0')
    env = g_getenv ("PHONEMIZER_ESPEAK_LIBRARY");

  if ((env != NULL && *env != '\0' && try_library (env)) ||
      try_library ("libespeak-ng.so.1") ||
      try_library ("libespeak-ng.so") ||
      try_extra_globs ())
    loaded = TRUE;
  else
    log_warning ("No espeak-ng command and no libespeak-ng library found");
  return loaded;
}

/* Caller holds `lock`.  Same strategy as the espeak-ng command: try the
 * name first, then treat it as a language ("en-gb").  Returns FALSE if no
 * voice could be selected - the library must not be used then (it crashes
 * when asked to translate text without a voice). */
static gboolean
select_voice (const char *voice)
{
  if (voice == NULL)
    voice = "en-us";
  if (g_strcmp0 (voice, current_voice) == 0)
    return TRUE;

  gboolean ok = api.SetVoiceByName (voice) == 0;
  if (!ok) {
    EspeakVoice props = { 0 };
    props.languages = voice;
    ok = api.SetVoiceByProperties (&props) == 0;
  }
  if (!ok) {
    log_warning ("libespeak-ng could not select voice %s", voice);
    g_clear_pointer (&current_voice, g_free);
    return FALSE;
  }
  g_free (current_voice);
  current_voice = g_strdup (voice);
  return TRUE;
}

const char *
wb_espeak_lib_path (void)
{
  g_mutex_lock (&lock);
  const char *p = ensure_loaded () ? lib_path : NULL;
  g_mutex_unlock (&lock);
  return p;
}

char *
wb_espeak_lib_ipa (const char *text, const char *voice)
{
  if (text == NULL || *text == '\0')
    return NULL;

  g_mutex_lock (&lock);
  if (!ensure_loaded ()) {
    g_mutex_unlock (&lock);
    return NULL;
  }
  if (!select_voice (voice)) {
    g_mutex_unlock (&lock);
    return NULL;
  }

  /* Same as `espeak-ng --ipa=3`: IPA, with a zero-width joiner (U+200D)
   * between the parts of multi-letter phonemes. */
  int mode = PHONEMES_IPA | PHONEMES_TIE | (0x200d << 8);

  GString *out = g_string_new (NULL);
  const void *ptr = text;
  for (int guard = 0; ptr != NULL && guard < 256; guard++) {
    const char *chunk = api.TextToPhonemes (&ptr, CHARS_UTF8, mode);
    if (chunk != NULL && *chunk != '\0') {
      if (out->len > 0)
        g_string_append_c (out, '\n');
      g_string_append (out, chunk);
    }
  }
  g_mutex_unlock (&lock);

  if (out->len == 0) {
    g_string_free (out, TRUE);
    return NULL;
  }
  return g_string_free (out, FALSE);
}

/* ---------------------------------------------------------------------- */
/* Speaking: render to PCM with the library, play it with a system tool.  */
/* ---------------------------------------------------------------------- */

static gboolean
write_wav (const char *path, const guint8 *data, gsize len, int rate)
{
  guint8 h[44];
  guint32 v;
  guint16 w;
  memcpy (h, "RIFF", 4);          v = GUINT32_TO_LE ((guint32) (36 + len)); memcpy (h + 4, &v, 4);
  memcpy (h + 8, "WAVEfmt ", 8);  v = GUINT32_TO_LE (16);                   memcpy (h + 16, &v, 4);
  w = GUINT16_TO_LE (1);          memcpy (h + 20, &w, 2);   /* PCM  */
  w = GUINT16_TO_LE (1);          memcpy (h + 22, &w, 2);   /* mono */
  v = GUINT32_TO_LE ((guint32) rate);      memcpy (h + 24, &v, 4);
  v = GUINT32_TO_LE ((guint32) rate * 2);  memcpy (h + 28, &v, 4);
  w = GUINT16_TO_LE (2);          memcpy (h + 32, &w, 2);   /* block align */
  w = GUINT16_TO_LE (16);         memcpy (h + 34, &w, 2);   /* bits        */
  memcpy (h + 36, "data", 4);     v = GUINT32_TO_LE ((guint32) len); memcpy (h + 40, &v, 4);

  g_autoptr (GByteArray) all = g_byte_array_sized_new (44 + (guint) len);
  g_byte_array_append (all, h, sizeof h);
  g_byte_array_append (all, data, (guint) len);
  g_autoptr (GError) err = NULL;
  if (!g_file_set_contents (path, (const char *) all->data, (gssize) all->len, &err)) {
    log_error ("Could not write speech file: %s", err->message);
    return FALSE;
  }
  return TRUE;
}

static int preferred_player = 0;   /* index of the last player that worked */

static gboolean
play_audio_file (const char *path)
{
  const char *const players[][6] = {
    { "pw-play", NULL },
    { "paplay",  NULL },
    { "aplay",   "-q", NULL },
    { "ffplay",  "-nodisp", "-autoexit", "-loglevel", "quiet", NULL },
  };
  const int n = G_N_ELEMENTS (players);

  for (int k = 0; k < n; k++) {
    int idx = (preferred_player + k) % n;
    GPtrArray *argv = g_ptr_array_new ();
    for (const char *const *a = players[idx]; *a != NULL; a++)
      g_ptr_array_add (argv, (gpointer) *a);
    g_ptr_array_add (argv, (gpointer) path);
    g_ptr_array_add (argv, NULL);

    gint status = 0;
    g_autoptr (GError) err = NULL;
    gboolean spawned = g_spawn_sync (NULL, (char **) argv->pdata, NULL,
                                     G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                                     G_SPAWN_STDERR_TO_DEV_NULL | G_SPAWN_STDIN_FROM_DEV_NULL,
                                     NULL, NULL, NULL, NULL, &status, &err);
    g_ptr_array_free (argv, TRUE);

    if (spawned && g_spawn_check_wait_status (status, NULL)) {
      preferred_player = idx;
      return TRUE;
    }
    /* not installed, or it couldn't reach the sound server: try the next one */
  }
  return FALSE;
}

gboolean
wb_espeak_lib_play_file (const char *path)
{
  if (path == NULL || *path == '\0')
    return FALSE;
  return play_audio_file (path);
}

/* Raw espeak output starts and ends abruptly. Played as-is, the first
 * syllable is often clipped while the sound server wakes the audio device, and
 * the hard edges produce clicks - both read as "breaking" and "harsh" speech.
 * Add a short lead-in of silence, tiny fades, and a short tail. */
static GByteArray *
condition_pcm (const GByteArray *in, int rate)
{
  const gsize n = in->len / sizeof (gint16);
  const gsize lead = (gsize) rate * 150 / 1000;   /* 150 ms of silence */
  const gsize tail = (gsize) rate * 80 / 1000;    /*  80 ms of silence */
  const gsize fade_in = MAX ((gsize) rate * 6 / 1000, 1);
  const gsize fade_out = MAX ((gsize) rate * 20 / 1000, 1);

  GByteArray *out = g_byte_array_sized_new ((guint) ((lead + n + tail) * sizeof (gint16)));
  g_byte_array_set_size (out, (guint) ((lead + n + tail) * sizeof (gint16)));
  gint16 *dst = (gint16 *) out->data;
  const gint16 *src = (const gint16 *) in->data;
  memset (dst, 0, out->len);

  for (gsize i = 0; i < n; i++) {
    double g = 1.0;
    if (i < fade_in)
      g = (double) i / (double) fade_in;
    if (n - 1 - i < fade_out)
      g = MIN (g, (double) (n - 1 - i) / (double) fade_out);
    dst[lead + i] = (gint16) (src[i] * g);
  }
  return out;
}

gboolean
wb_espeak_lib_speak (const char *text, const char *voice, int speed, int pitch)
{
  if (text == NULL || *text == '\0')
    return FALSE;

  g_mutex_lock (&lock);
  if (!ensure_loaded ()) {
    g_mutex_unlock (&lock);
    return FALSE;
  }
  if (!select_voice (voice)) {
    g_mutex_unlock (&lock);
    return FALSE;
  }
  api.SetParameter (PARAM_RATE, speed, 0);
  api.SetParameter (PARAM_PITCH, CLAMP (pitch, 0, 99), 0);

  pcm = g_byte_array_new ();
  api.Synth (text, strlen (text) + 1, 0, POS_CHARACTER, 0,
             CHARS_UTF8 | FLAG_PHONEMES | FLAG_ENDPAUSE, NULL, NULL);
  api.Synchronize ();
  GByteArray *samples = g_steal_pointer (&pcm);
  int rate = sample_rate;
  g_mutex_unlock (&lock);

  gboolean ok = FALSE;
  if (samples->len > 0) {
    GByteArray *padded = condition_pcm (samples, rate);
    g_byte_array_free (samples, TRUE);
    samples = padded;
    g_autofree char *tmp = NULL;
    g_autoptr (GError) err = NULL;
    int fd = g_file_open_tmp ("dictionary-speech-XXXXXX.wav", &tmp, &err);
    if (fd < 0) {
      log_error ("Could not create speech file: %s", err->message);
    } else {
      close (fd);
      if (write_wav (tmp, samples->data, samples->len, rate)) {
        ok = play_audio_file (tmp);
        if (!ok)
          log_error ("No audio player worked (tried pw-play, paplay, aplay, ffplay)");
      }
      g_unlink (tmp);
    }
  }
  g_byte_array_free (samples, TRUE);
  return ok;
}
