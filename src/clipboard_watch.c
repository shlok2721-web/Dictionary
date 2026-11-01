#include "clipboard_watch.h"

#include <gio/gio.h>
#include <string.h>

#include "settings.h"
#include "utils.h"

#define MAX_CANDIDATE_LENGTH 48
#define MAX_CANDIDATE_WORDS  2

/* The same text arriving again within this window is one gesture seen twice
   (select-then-Ctrl+C, or duplicate compositor events), not a new lookup. */
#define DUPLICATE_WINDOW_US  (1500 * 1000)
/* `wl-paste --watch` fires once immediately for whatever is already
   selected/copied; ignore ticks this soon after starting so enabling lookup
   doesn't pop up the old contents. */
#define STARTUP_GRACE_US     (700 * 1000)

/* One Source per thing we can watch: the regular clipboard (Ctrl+C) and the
   primary selection (whatever is highlighted). */
typedef struct {
  gboolean          primary;
  /* Wayland background backend: `wl-paste [--primary] --watch` (wlr/ext
     data-control). GDK's "changed" signal on Wayland only fires while this
     app has keyboard focus, so a tray/background app would never see
     copies or selections made in other windows. */
  GSubprocess      *watcher;
  GDataInputStream *watcher_out;
  GCancellable     *cancel;
  guint             debounce_id;   /* coalesces rapid selection changes */
  gint64            started_us;    /* when this watcher was started */

  GdkClipboard     *gdk;           /* fallback when wl-paste is unavailable */
  gulong            changed_handler;
} Source;

typedef struct {
  Source   sources[2];             /* [0] clipboard, [1] primary selection */
  gboolean active;
  char    *last_seen;
  gint64   last_seen_us;

  WbClipboardWatchCallback cb;
  gpointer                 user_data;

  WbClipboardWatchStateCallback state_cb;
  gpointer                      state_cb_data;
} WatchState;

static WatchState state = { .sources = { { .primary = FALSE }, { .primary = TRUE } } };

static gboolean
looks_like_a_lookup_candidate (const char *text)
{
  if (wb_str_is_blank (text))
    return FALSE;

  gsize len = strlen (text);
  if (len == 0 || len > MAX_CANDIDATE_LENGTH)
    return FALSE;

  /* Multi-line clipboard contents are essentially never "a word someone
     wants defined" — reject outright rather than guessing which line. */
  if (strchr (text, '\n') != NULL || strchr (text, '\r') != NULL)
    return FALSE;

  /* Filter out URLs, paths and anything else with path/URL punctuation —
     these are copied constantly and are never dictionary lookups. */
  if (strstr (text, "://") != NULL || strchr (text, '/') != NULL ||
      strchr (text, '\\') != NULL || strchr (text, '@') != NULL)
    return FALSE;

  /* Count whitespace-separated words (runs of spaces/tabs count once). */
  guint words = 0;
  gboolean in_word = FALSE;
  gboolean has_alpha = FALSE;
  for (const char *p = text; *p; p++) {
    if (*p == ' ' || *p == '\t') {
      in_word = FALSE;
      continue;
    }
    if (!in_word) {
      words++;
      in_word = TRUE;
    }
    if (g_ascii_isalpha (*p))
      has_alpha = TRUE;
  }
  if (words == 0 || words > MAX_CANDIDATE_WORDS || !has_alpha)
    return FALSE;

  return TRUE;
}

static void
deliver_text (const char *text)
{
  g_autofree char *stripped = wb_str_strip (text);
  if (!looks_like_a_lookup_candidate (stripped))
    return;

  /* Shared across both sources: highlighting a word and then Ctrl+C-ing the
     same word should open the popup once, not twice. */
  gint64 now = g_get_monotonic_time ();
  if (state.last_seen != NULL &&
      now - state.last_seen_us < DUPLICATE_WINDOW_US &&
      g_ascii_strcasecmp (state.last_seen, stripped) == 0)
    return;

  g_free (state.last_seen);
  state.last_seen = g_strdup (stripped);
  state.last_seen_us = now;

  if (state.cb)
    state.cb (stripped, state.user_data);
}

/* ---- GDK fallback ------------------------------------------------------ */

static void
on_text_ready (GObject *source, GAsyncResult *res, gpointer user_data)
{
  g_autoptr (GError) err = NULL;
  g_autofree char *text = gdk_clipboard_read_text_finish (GDK_CLIPBOARD (source), res, &err);

  if (!state.active || text == NULL)
    return;
  deliver_text (text);
}

static void
on_gdk_changed (GdkClipboard *clipboard, gpointer user_data)
{
  if (!state.active)
    return;
  gdk_clipboard_read_text_async (clipboard, NULL, on_text_ready, NULL);
}

static void
attach_gdk_fallback (Source *src)
{
  GdkDisplay *display = gdk_display_get_default ();
  if (display == NULL || src->changed_handler != 0)
    return;
  src->gdk = src->primary ? gdk_display_get_primary_clipboard (display)
                          : gdk_display_get_clipboard (display);
  src->changed_handler = g_signal_connect (src->gdk, "changed", G_CALLBACK (on_gdk_changed), NULL);
}

/* ---- wl-paste backend -------------------------------------------------- */

static void
on_wlpaste_read (GObject *source, GAsyncResult *res, gpointer user_data)
{
  g_autoptr (GError) err = NULL;
  g_autofree char *out = NULL;

  /* `source` is borrowed from the async operation; the reference taken by
     g_subprocess_newv() is released by the code that started it. */
  g_subprocess_communicate_utf8_finish (G_SUBPROCESS (source), res, &out, NULL, &err);
  if (err == NULL && out != NULL && state.active)
    deliver_text (out);
}

static gboolean
read_via_wlpaste_cb (gpointer data)
{
  Source *src = data;
  src->debounce_id = 0;
  if (!state.active)
    return G_SOURCE_REMOVE;

  g_autoptr (GError) err = NULL;
  const char *argv_clip[]    = { "wl-paste", "--no-newline", "--type", "text", NULL };
  const char *argv_primary[] = { "wl-paste", "--primary", "--no-newline", "--type", "text", NULL };
  g_autoptr (GSubprocess) p = g_subprocess_newv (src->primary ? argv_primary : argv_clip,
                                                 G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                                 G_SUBPROCESS_FLAGS_STDERR_SILENCE, &err);
  if (p == NULL) {
    log_warning ("Lookup watch: could not run wl-paste: %s", err->message);
    return G_SOURCE_REMOVE;
  }
  g_subprocess_communicate_utf8_async (p, NULL, src->cancel, on_wlpaste_read, NULL);
  return G_SOURCE_REMOVE;
}

static void
schedule_read (Source *src)
{
  /* A mouse drag can change the primary selection many times a second; wait
     for it to settle. Copies are discrete events and are read right away. */
  guint delay_ms = src->primary ? 180 : 0;
  g_clear_handle_id (&src->debounce_id, g_source_remove);
  if (delay_ms == 0)
    read_via_wlpaste_cb (src);
  else
    src->debounce_id = g_timeout_add (delay_ms, read_via_wlpaste_cb, src);
}

static void
on_watcher_line (GObject *source, GAsyncResult *res, gpointer user_data)
{
  Source *src = user_data;
  g_autoptr (GError) err = NULL;
  gsize len = 0;
  g_autofree char *line = g_data_input_stream_read_line_finish (G_DATA_INPUT_STREAM (source),
                                                                res, &len, &err);
  if (line == NULL) {
    if (err != NULL && g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      return;
    if (!state.active)
      return;
    /* wl-paste died (compositor without data-control, killed, etc.). Don't
       leave the UI claiming lookup is on while nothing is listening: drop
       the dead watcher and fall back to GDK's own signal. */
    log_warning ("Lookup watch (%s): wl-paste watcher ended; falling back to GDK "
                "(only sees changes made while this app has focus)",
                src->primary ? "selection" : "clipboard");
    g_clear_object (&src->watcher_out);
    g_clear_object (&src->watcher);
    g_clear_object (&src->cancel);
    attach_gdk_fallback (src);
    return;
  }
  if (state.active && g_get_monotonic_time () - src->started_us >= STARTUP_GRACE_US)
    schedule_read (src);
  g_data_input_stream_read_line_async (G_DATA_INPUT_STREAM (source), G_PRIORITY_DEFAULT,
                                       src->cancel, on_watcher_line, src);
}

static gboolean
start_wlpaste_watcher (Source *src)
{
  if (g_getenv ("WAYLAND_DISPLAY") == NULL || g_find_program_in_path ("wl-paste") == NULL)
    return FALSE;

  g_autoptr (GError) err = NULL;
  /* Each change makes wl-paste run `echo c` once; we only use that as a
     "something changed" tick and then fetch the text ourselves, which keeps
     multi-line contents from breaking line framing. */
  const char *argv_clip[]    = { "wl-paste", "--type", "text", "--watch", "echo", "c", NULL };
  const char *argv_primary[] = { "wl-paste", "--primary", "--type", "text", "--watch", "echo", "c", NULL };
  src->watcher = g_subprocess_newv (src->primary ? argv_primary : argv_clip,
                                    G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                    G_SUBPROCESS_FLAGS_STDERR_SILENCE, &err);
  if (src->watcher == NULL) {
    log_warning ("Lookup watch: wl-paste --watch failed: %s", err->message);
    return FALSE;
  }
  src->started_us = g_get_monotonic_time ();
  src->cancel = g_cancellable_new ();
  src->watcher_out = g_data_input_stream_new (g_subprocess_get_stdout_pipe (src->watcher));
  g_data_input_stream_read_line_async (src->watcher_out, G_PRIORITY_DEFAULT,
                                       src->cancel, on_watcher_line, src);
  log_warning ("Lookup watch: %s started (wl-paste data-control backend)",
              src->primary ? "selection" : "clipboard");
  return TRUE;
}

static void
start_source (Source *src)
{
  if (start_wlpaste_watcher (src))
    return;
  log_warning ("Lookup watch: wl-paste not available; falling back to GDK, which on Wayland "
              "only sees changes made while this app has focus. Install wl-clipboard.");
  attach_gdk_fallback (src);
}

static void
stop_source (Source *src)
{
  g_clear_handle_id (&src->debounce_id, g_source_remove);
  if (src->cancel != NULL)
    g_cancellable_cancel (src->cancel);
  if (src->watcher != NULL)
    g_subprocess_force_exit (src->watcher);
  g_clear_object (&src->watcher_out);
  g_clear_object (&src->watcher);
  g_clear_object (&src->cancel);
  if (src->gdk != NULL && src->changed_handler != 0)
    g_signal_handler_disconnect (src->gdk, src->changed_handler);
  src->changed_handler = 0;
  src->gdk = NULL;
}

/* ---- public API -------------------------------------------------------- */

void
wb_clipboard_watch_set_state_callback (WbClipboardWatchStateCallback cb, gpointer user_data)
{
  state.state_cb = cb;
  state.state_cb_data = user_data;
}

static void
notify_state (void)
{
  if (state.state_cb)
    state.state_cb (state.active, state.state_cb_data);
}

void
wb_clipboard_watch_start (WbClipboardWatchCallback cb, gpointer user_data)
{
  if (state.active)
    return;

  if (gdk_display_get_default () == NULL) {
    log_error ("Lookup watch: no display available");
    return;
  }

  state.cb = cb;
  state.user_data = user_data;
  state.active = TRUE;

  WbLookupSource mode = wb_settings_get_lookup_source (wb_settings_get ());
  if (mode == WB_LOOKUP_CLIPBOARD || mode == WB_LOOKUP_BOTH)
    start_source (&state.sources[0]);
  if (mode == WB_LOOKUP_SELECTION || mode == WB_LOOKUP_BOTH)
    start_source (&state.sources[1]);

  notify_state ();
}

void
wb_clipboard_watch_stop (void)
{
  if (!state.active)
    return;
  stop_source (&state.sources[0]);
  stop_source (&state.sources[1]);
  state.active = FALSE;
  g_clear_pointer (&state.last_seen, g_free);
  log_info ("Lookup watch: stopped");
  notify_state ();
}

gboolean
wb_clipboard_watch_is_active (void)
{
  return state.active;
}
