#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Fallback used only when the `espeak-ng` command is NOT on PATH.
 *
 * Instead of running the command, libespeak-ng is loaded at run time with
 * dlopen(), so there is no build-time dependency on it.  Search order:
 *   1. $WB_ESPEAK_LIBRARY or $PHONEMIZER_ESPEAK_LIBRARY (full path to the .so)
 *   2. the system library (libespeak-ng.so.1 / libespeak-ng.so)
 *   3. EXTRA_LIB_GLOBS in espeak_lib.c (a Python virtualenv's espeakng_loader)
 *
 * All functions are thread-safe and return NULL / FALSE if no library could
 * be loaded. */

/* IPA for TEXT in VOICE ("en-us", "en-gb"), formatted like
 * `espeak-ng --ipa=3 -q`.  Newly allocated, or NULL. */
char       *wb_espeak_lib_ipa   (const char *text, const char *voice);

/* Say TEXT (plain text, or [[espeak phonemes]]) aloud.  SPEED is in words
 * per minute, PITCH is 0-99 (50 = unchanged).  Blocks until playback ends.
 * Needs one of pw-play, paplay, aplay or ffplay for the actual sound output. */
gboolean    wb_espeak_lib_speak (const char *text, const char *voice, int speed, int pitch);

/* Path of the library in use (loads it on first call), or NULL. */
const char *wb_espeak_lib_path  (void);

/* Play an existing local audio file (wav, mp3, ogg, ...) with the same
 * player chain wb_espeak_lib_speak() uses (pw-play, paplay, aplay, ffplay),
 * agnostic of what produced it - used to play recorded pronunciations.
 * Blocks until playback ends. */
gboolean    wb_espeak_lib_play_file (const char *path);

G_END_DECLS
