#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/*
 * A small, borderless lookup window pinned to the top-right corner of the
 * screen — the "glance at a definition without breaking stride" surface
 * used by the tray icon and the clipboard watcher. It's sized as a
 * percentage of the monitor it appears on (default 20%, configurable in
 * Preferences) rather than a fixed pixel size, so it scales sensibly
 * across small laptop panels and large external monitors alike.
 *
 * Positioning uses gtk4-layer-shell (the wlr-layer-shell protocol), which
 * is the only way a Wayland client can anchor itself to a screen edge at
 * all — plain GTK top-level windows cannot request their own position on
 * Wayland. This is also exactly the mechanism waybar itself, mako,
 * wlogout and similar panel-adjacent tools use, so it behaves
 * consistently with the rest of a Wayland desktop. On an X11 session the
 * window falls back to being centred near the top-right of the screen
 * with ordinary geometry calls.
 */

typedef void (*WbPopupOpenRequestCallback) (const char *term, gpointer user_data);

/* Call once at startup. `open_request_cb` is invoked when the user asks to
   open the full application window for a term (e.g. "View full entry"). */
void wb_popup_window_init (WbPopupOpenRequestCallback open_request_cb, gpointer user_data);

/* Looks `term` up and shows the popup positioned at the top-right of the
   monitor under the pointer, sized to `popup_size_percent` of that
   monitor's resolution. Safe to call repeatedly; reuses one window. */
void wb_popup_window_show_term (const char *term);

/* Shows/hides without changing content (used for the tray left-click
   toggle — if a lookup is already on screen, click again to dismiss it). */
void wb_popup_window_toggle (void);
void wb_popup_window_hide (void);
gboolean wb_popup_window_is_visible (void);

/* Call once from the application's shutdown handler: destroys the popup,
   cancels its timers and closes its database connection. */
void wb_popup_window_shutdown (void);

/* Shared clipboard-watch callback: shows the popup for `term` if and only
   if "Auto-Show Mini Window on Copy" is currently on. Exported (rather
   than kept as a static callback in main.c) so every place that can
   start/stop the clipboard watcher — main.c at startup, the tray menu,
   and the Preferences window — passes the exact same behavior instead of
   each re-implementing the auto-show check. `user_data` is unused. */
void wb_popup_window_handle_clipboard_term (const char *term, gpointer user_data);

G_END_DECLS
