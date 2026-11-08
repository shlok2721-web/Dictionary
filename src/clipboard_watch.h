#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/*
 * Watches the clipboard and/or the primary selection (see Preferences →
 * Lookup Trigger). Without wl-clipboard this uses GdkClipboard's native
 * "changed" signal (no polling thread needed) and reports candidate
 * lookup terms: short, single-line, word-or-phrase-shaped text. Anything
 * that looks like code, a URL, a path, or a block of prose is filtered
 * out before the callback ever fires, both to avoid spamming the popup
 * and because most of that text was never meant to be "looked up".
 */

typedef void (*WbClipboardWatchCallback) (const char *term, gpointer user_data);

void     wb_clipboard_watch_start (WbClipboardWatchCallback cb, gpointer user_data);
void     wb_clipboard_watch_stop  (void);
gboolean wb_clipboard_watch_is_active (void);

/* Called after every start/stop with the new state, so UI that mirrors it
   (the tray icon) stays in sync no matter who toggled it. Pass NULL to
   clear. */
typedef void (*WbClipboardWatchStateCallback) (gboolean active, gpointer user_data);
void wb_clipboard_watch_set_state_callback (WbClipboardWatchStateCallback cb, gpointer user_data);

G_END_DECLS
