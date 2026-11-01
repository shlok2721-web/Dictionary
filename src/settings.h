#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  WB_ACCENT_US = 0,
  WB_ACCENT_GB = 1,
  WB_ACCENT_COUNT
} WbPronunciationAccent;

const char            *wb_accent_code         (WbPronunciationAccent a);
const char            *wb_accent_display_name (WbPronunciationAccent a);
WbPronunciationAccent  wb_accent_from_code    (const char *code);
WbPronunciationAccent  wb_accent_from_index   (int index);
static inline int      wb_accent_index        (WbPronunciationAccent a) { return (int) a; }

/* Voice used for spoken pronunciation (text-to-speech). espeak-ng's own
 * default voice is a flat, fairly low monotone ("mechanical"); the other
 * three select one of espeak-ng's built-in variants, appended to the
 * accent's voice id (e.g. "en-us+f3"). */
typedef enum {
  WB_VOICE_FEMALE      = 0,   /* +f3 - clear female voice, the default   */
  WB_VOICE_FEMALE_SOFT = 1,   /* +f2 - softer, slightly lower female     */
  WB_VOICE_MALE        = 2,   /* +m3 - male voice                        */
  WB_VOICE_DEFAULT     = 3,   /* espeak-ng's own plain default voice     */
  WB_VOICE_COUNT
} WbVoiceStyle;

const char   *wb_voice_display_name (WbVoiceStyle v);
const char   *wb_voice_variant      (WbVoiceStyle v);   /* "+f3", "+f2", "+m3", or "" */
const char   *wb_voice_code         (WbVoiceStyle v);   /* stable id, for the config file */
WbVoiceStyle  wb_voice_from_code    (const char *code);
WbVoiceStyle  wb_voice_from_index   (int index);
static inline int wb_voice_index (WbVoiceStyle v) { return (int) v; }

/* What triggers an automatic lookup while "word lookup" is on. */
typedef enum {
  WB_LOOKUP_CLIPBOARD = 0,   /* text you copy (Ctrl+C)                       */
  WB_LOOKUP_SELECTION = 1,   /* text you merely highlight (primary selection) */
  WB_LOOKUP_BOTH      = 2,
  WB_LOOKUP_COUNT
} WbLookupSource;

typedef struct _WbSettings WbSettings;

WbSettings *wb_settings_get (void);

gboolean wb_settings_get_live_search          (WbSettings *s);
void     wb_settings_set_live_search          (WbSettings *s, gboolean v);
gboolean wb_settings_get_double_click         (WbSettings *s);
void     wb_settings_set_double_click         (WbSettings *s, gboolean v);
gboolean wb_settings_get_auto_paste_on_launch (WbSettings *s);
void     wb_settings_set_auto_paste_on_launch (WbSettings *s, gboolean v);
WbPronunciationAccent wb_settings_get_pronunciations_accent (WbSettings *s);
void     wb_settings_set_pronunciations_accent (WbSettings *s, WbPronunciationAccent a);

/* Speech: which built-in voice to speak with, and its pitch (espeak-ng's
 * own 0-99 scale, 50 = unchanged; defaults a bit above that, since the
 * stock pitch on top of the female variant still reads as flat). */
WbVoiceStyle wb_settings_get_voice_style (WbSettings *s);
void     wb_settings_set_voice_style     (WbSettings *s, WbVoiceStyle v);
int      wb_settings_get_voice_pitch     (WbSettings *s);
void     wb_settings_set_voice_pitch     (WbSettings *s, int v);

/* When available, play a recorded human pronunciation (pron.audio_url)
 * instead of synthesizing one. Off by default: it's the only setting that
 * makes Dictionary touch the network, downloading and caching the
 * recording under the data directory the first time a given word is
 * played this way. */
gboolean wb_settings_get_use_recorded_audio (WbSettings *s);
void     wb_settings_set_use_recorded_audio (WbSettings *s, gboolean v);

gboolean wb_settings_get_gtk_dark_ui (WbSettings *s);
void     wb_settings_set_gtk_dark_ui (WbSettings *s, gboolean v);

char   **wb_settings_get_history   (WbSettings *s);
void     wb_settings_set_history   (WbSettings *s, const char *const *terms);
void     wb_settings_clear_history (WbSettings *s);

char   **wb_settings_get_favorites   (WbSettings *s);
void     wb_settings_set_favorites   (WbSettings *s, const char *const *terms);
void     wb_settings_add_favorite    (WbSettings *s, const char *term);
void     wb_settings_remove_favorite (WbSettings *s, const char *term);
gboolean wb_settings_is_favorite     (WbSettings *s, const char *term);

int      wb_settings_get_window_width  (WbSettings *s);
void     wb_settings_set_window_width  (WbSettings *s, int v);
int      wb_settings_get_window_height (WbSettings *s);
void     wb_settings_set_window_height (WbSettings *s, int v);

/* Tray icon / background service / clipboard lookup. */
gboolean wb_settings_get_tray_enabled            (WbSettings *s);
void     wb_settings_set_tray_enabled            (WbSettings *s, gboolean v);
gboolean wb_settings_get_run_in_background       (WbSettings *s);
void     wb_settings_set_run_in_background       (WbSettings *s, gboolean v);
gboolean wb_settings_get_clipboard_watch_enabled (WbSettings *s);
void     wb_settings_set_clipboard_watch_enabled (WbSettings *s, gboolean v);
WbLookupSource wb_settings_get_lookup_source (WbSettings *s);
void     wb_settings_set_lookup_source (WbSettings *s, WbLookupSource v);
gboolean wb_settings_get_auto_show_popup         (WbSettings *s);
void     wb_settings_set_auto_show_popup         (WbSettings *s, gboolean v);
int      wb_settings_get_popup_size_percent      (WbSettings *s);
void     wb_settings_set_popup_size_percent      (WbSettings *s, int v);

/* How long the clipboard-triggered mini popup stays open before it
 * auto-hides, in seconds. Pointer activity over the popup resets this
 * countdown rather than letting it expire mid-read. */
int      wb_settings_get_popup_autohide_seconds  (WbSettings *s);
void     wb_settings_set_popup_autohide_seconds  (WbSettings *s, int v);

/* Optional user-chosen location of the Krumeto SQLite database
 * (krumeto-<version>.db).  NULL / "" means "automatic": look in the usual
 * install locations.  The getter returns a newly allocated string, or NULL
 * when no custom path is set.  Changes apply the next time Dictionary starts. */
char    *wb_settings_get_database_path (WbSettings *s);
void     wb_settings_set_database_path (WbSettings *s, const char *path);

void wb_settings_save_state (WbSettings *s, const char *const *history,
                             int width, int height);

void wb_settings_freeze (WbSettings *s);
void wb_settings_thaw   (WbSettings *s);
void wb_settings_reset_to_defaults (WbSettings *s);

G_END_DECLS
