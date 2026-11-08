#pragma once

#include <glib.h>

G_BEGIN_DECLS

/*
 * WbTray publishes a StatusNotifierItem (SNI) on the session D-Bus so the
 * app can show an icon in any SNI-compatible tray/status-bar host —
 * waybar's tray module, GNOME Shell's "AppIndicator Support" extension,
 * Plasma's system tray, etc. GTK4 dropped GtkStatusIcon/the old X11 tray
 * protocol entirely, and on Wayland there is no X11 systray to fall back
 * to, so SNI (spoken by every modern Wayland panel) is the only portable
 * way to put an icon there. It's implemented directly over GDBus (part of
 * GIO, already a dependency) rather than via libappindicator, so no extra
 * library is required and there's no risk of it pulling in a conflicting
 * GTK3 runtime alongside this GTK4 app.
 *
 * A tiny, static com.canonical.dbusmenu menu is published alongside the
 * item so hosts that only show a menu on right-click (waybar included)
 * have something to display. Left-click (Activate) toggles clipboard
 * lookup on/off and the icon switches between an active and an inactive
 * look; the mini lookup window itself is only ever shown by a clipboard
 * copy while lookup is on.
 */

typedef struct _WbTray WbTray;

typedef struct {
  /* "Open Dictionary" menu entry: raise the full application window. */
  void (*on_open_window) (gpointer user_data);

  /* "Watch Clipboard" menu entry was toggled to `enable`. */
  void (*on_set_clipboard_watch) (gpointer user_data, gboolean enable);

  /* "Keep Running in Background" menu entry was toggled to `enable`. */
  void (*on_set_run_in_background) (gpointer user_data, gboolean enable);

  /* "Quit" menu entry: shut the application down for real. */
  void (*on_quit) (gpointer user_data);

  /* Live state queried each time the menu is (re)built, so it always
     reflects reality even if settings changed elsewhere (e.g. from the
     Preferences window). */
  gboolean (*get_clipboard_watch) (gpointer user_data);
  gboolean (*get_run_in_background) (gpointer user_data);

  gpointer user_data;
} WbTrayCallbacks;

/* Starts publishing the tray icon asynchronously (acquiring the D-Bus name
   and registering with whichever StatusNotifierWatcher is running happens
   in the background); returns immediately with a handle. If no watcher is
   present on the bus the icon simply never appears anywhere, the rest of
   the app is unaffected, and this is logged once rather than treated as a
   fatal error. */
WbTray *wb_tray_new (const WbTrayCallbacks *callbacks);

/* Call whenever clipboard-watch state may have changed (from anywhere:
   tray, Preferences, startup) so the icon artwork, tooltip and menu
   checkmark are re-read by the host. Safe to call with NULL. */
void wb_tray_refresh (WbTray *tray);

void wb_tray_free (WbTray *tray);

G_END_DECLS
