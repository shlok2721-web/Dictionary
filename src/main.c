#include <adwaita.h>
#include <glib/gi18n.h>
#include <glib-unix.h>
#include <locale.h>

#include "base.h"
#include "clipboard_watch.h"
#include "constants.h"
#include "popup_window.h"
#include "settings.h"
#include "tray.h"
#include "utils.h"
#include "window.h"

#ifndef APP_ID
#define APP_ID "dev.silvanus.dictionary"
#endif
#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

#define DICTIONARY_TYPE_APPLICATION (dictionary_application_get_type ())
G_DECLARE_FINAL_TYPE (DictionaryApplication, dictionary_application, DICTIONARY, APPLICATION, AdwApplication)

struct _DictionaryApplication {
  AdwApplication parent_instance;

  char    *app_id;
  char    *version;
  gboolean development_mode;

  char    *lookup_term;
  gboolean auto_paste_requested;
  DictionaryWindow *win;

  /* Tray icon / background service / clipboard lookup (see tray.h,
     clipboard_watch.h, popup_window.h). `services_started` guards the
     one-time setup so repeat activations (second launches, tray clicks)
     don't re-create the tray icon or re-arm the clipboard watcher.
     `background_requested` is only ever true for the very first
     activation of a process started with --background: it suppresses
     opening the main window on that one launch, nothing more. */
  WbTray   *tray;
  gboolean  services_started;
  gboolean  background_requested;
};

G_DEFINE_FINAL_TYPE (DictionaryApplication, dictionary_application, ADW_TYPE_APPLICATION)

static void
on_about (GSimpleAction *action, GVariant *param, gpointer data)
{
  DictionaryApplication *self = data;
  AdwDialog *about = adw_about_dialog_new ();
  AdwAboutDialog *a = ADW_ABOUT_DIALOG (about);

  adw_about_dialog_set_application_icon (a, self->app_id);
  adw_about_dialog_set_application_name (a, _("Dictionary"));
  adw_about_dialog_set_version (a, self->version);
  adw_about_dialog_set_comments (a,
    _("An offline dictionary for GNOME, backed by the Open English WordNet. "
      "Look up definitions, synonyms, antonyms and pronunciations without a "
      "network connection."));
  adw_about_dialog_set_website (a, "https://github.com/shlok2721-web/Dictionary");
  adw_about_dialog_set_issue_url (a, "https://github.com/shlok2721-web/Dictionary/issues");
  adw_about_dialog_set_copyright (a, "\u00a9 2026 Dictionary contributors");
  adw_dialog_present (about, self->win ? GTK_WIDGET (self->win) : NULL);
}

static void
on_quit (GSimpleAction *action, GVariant *param, gpointer data)
{
  g_application_quit (G_APPLICATION (data));
}

/* Raises the main window, creating it if this is the first time it's
   needed (e.g. the app was started with --background, or the window was
   closed while "Keep Running in Background" kept the process alive).
   Used by the tray's "Open Dictionary" entry and by the popup's
   "View Full Entry" button. */
static void
present_window_plain (DictionaryApplication *self)
{
  GtkWindow *active = gtk_application_get_active_window (GTK_APPLICATION (self));

  if (active == NULL) {
    DictionaryWindow *win = dictionary_window_new (GTK_APPLICATION (self), _("Dictionary"), "", FALSE);
    if (self->development_mode)
      gtk_widget_add_css_class (GTK_WIDGET (win), "devel");
    g_set_weak_pointer (&self->win, win);
    active = GTK_WINDOW (win);
  } else {
    g_set_weak_pointer (&self->win, DICTIONARY_WINDOW (active));
  }
  gtk_window_present (active);
}

static void
on_popup_open_request (const char *term, gpointer user_data)
{
  DictionaryApplication *self = user_data;
  present_window_plain (self);
  if (self->win != NULL)
    dictionary_window_trigger_search (self->win, term);
}

static void
on_clipboard_watch_state_changed (gboolean active, gpointer user_data)
{
  DictionaryApplication *self = user_data;
  wb_tray_refresh (self->tray);
}

static void
on_tray_open_window (gpointer user_data)
{
  present_window_plain (DICTIONARY_APPLICATION (user_data));
}

static void
on_tray_set_clipboard_watch (gpointer user_data, gboolean enable)
{
  wb_settings_set_clipboard_watch_enabled (wb_settings_get (), enable);
  if (enable)
    wb_clipboard_watch_start (wb_popup_window_handle_clipboard_term, NULL);
  else
    wb_clipboard_watch_stop ();
}

static void
on_tray_set_run_in_background (gpointer user_data, gboolean enable)
{
  WbSettings *s = wb_settings_get ();
  gboolean previous = wb_settings_get_run_in_background (s);
  wb_settings_set_run_in_background (s, enable);
  /* GApplication's hold/release is a plain counter: only (de)apply it on
     an actual change, or a second toggle in the same direction would
     double up and leave the app unkillable (or quittable too early). */
  if (enable && !previous)
    g_application_hold (G_APPLICATION (user_data));
  else if (!enable && previous)
    g_application_release (G_APPLICATION (user_data));
}

static void
on_tray_quit (gpointer user_data)
{
  /* g_application_quit() ignores the hold/use count entirely and shuts
     down right away, so there's no need to balance the hold above first. */
  g_application_quit (G_APPLICATION (user_data));
}

static gboolean
on_tray_get_clipboard_watch (gpointer user_data)
{
  return wb_settings_get_clipboard_watch_enabled (wb_settings_get ());
}

static gboolean
on_tray_get_run_in_background (gpointer user_data)
{
  return wb_settings_get_run_in_background (wb_settings_get ());
}

static void
setup_actions (DictionaryApplication *self)
{
  static const GActionEntry entries[] = {
    { "about", on_about, NULL, NULL, NULL },
    { "quit",  on_quit,  NULL, NULL, NULL },
  };
  g_action_map_add_action_entries (G_ACTION_MAP (self), entries, G_N_ELEMENTS (entries), self);

  /*
   * Accelerators below are all Ctrl (<Primary>)-based, mnemonic, and chosen
   * to be unlikely to collide with window-manager level bindings such as
   * Hyprland's defaults, which are almost always bound on the Super (Mod4)
   * key rather than Ctrl. "Preferences" was moved from Ctrl+, to Ctrl+P
   * (P for Preferences) at the maintainer's request; "Search selected" moved
   * to Ctrl+F (F for Find) to avoid the save-file association of Ctrl+S;
   * "Toggle sidebar" moved from F9 to Ctrl+B (B for sidebar/"bar", matching
   * the convention used by several GTK apps); "Toggle favorites" previously
   * had no shortcut at all and now has one for parity with the others.
   * "Toggle menu" is left on F10, which is the GNOME HIG convention for
   * opening an app's primary menu.
   */
  GtkApplication *app = GTK_APPLICATION (self);
  struct { const char *action; const char *accel; } accels[] = {
    { "app.quit",             "<Primary>q" },
    { "win.quit",             "<Primary>w" },
    { "win.search-selected",  "<Primary>f" },
    { "win.random-word",      "<Primary>r" },
    { "win.paste-search",     "<Primary><Shift>v" },
    { "win.preferences",      "<Primary>p" },
    { "win.toggle-sidebar",   "<Primary>b" },
    { "win.toggle-favorites", "<Primary>d" },
    { "win.toggle-menu",      "F10" },
  };
  for (gsize i = 0; i < G_N_ELEMENTS (accels); i++) {
    const char *list[] = { accels[i].accel, NULL };
    gtk_application_set_accels_for_action (app, accels[i].action, list);
  }
}

/* One-time setup for everything that should exist for the whole process
   lifetime regardless of whether/when the main window is open: the tray
   icon, the mini-popup machinery, the clipboard watcher, and the
   GApplication hold that keeps the process alive with no window open
   when "Keep Running in Background" is on. Safe to call on every
   activation; only runs once. */
static void
ensure_services_started (DictionaryApplication *self)
{
  if (self->services_started)
    return;
  self->services_started = TRUE;

  setup_actions (self);

  wb_popup_window_init (on_popup_open_request, self);

  if (wb_settings_get_tray_enabled (wb_settings_get ())) {
    WbTrayCallbacks cb = {
      .on_open_window           = on_tray_open_window,
      .on_set_clipboard_watch   = on_tray_set_clipboard_watch,
      .on_set_run_in_background = on_tray_set_run_in_background,
      .on_quit                  = on_tray_quit,
      .get_clipboard_watch      = on_tray_get_clipboard_watch,
      .get_run_in_background    = on_tray_get_run_in_background,
      .user_data                = self,
    };
    self->tray = wb_tray_new (&cb);
  }

  wb_clipboard_watch_set_state_callback (on_clipboard_watch_state_changed, self);
  if (wb_settings_get_clipboard_watch_enabled (wb_settings_get ()))
    wb_clipboard_watch_start (wb_popup_window_handle_clipboard_term, NULL);

  if (wb_settings_get_run_in_background (wb_settings_get ()))
    g_application_hold (G_APPLICATION (self));
}

static gboolean
quit_cb (gpointer data)
{
  g_application_quit (G_APPLICATION (data));
  return G_SOURCE_REMOVE;
}

static void
dictionary_application_startup (GApplication *app)
{
  g_application_set_resource_base_path (app, WB_RES_PATH);
  G_APPLICATION_CLASS (dictionary_application_parent_class)->startup (app);

  adw_style_manager_set_color_scheme (adw_style_manager_get_default (),
    wb_settings_get_gtk_dark_ui (wb_settings_get ()) ? ADW_COLOR_SCHEME_FORCE_DARK
                                                     : ADW_COLOR_SCHEME_DEFAULT);

  g_unix_signal_add (SIGINT, quit_cb, app);
  g_unix_signal_add (SIGTERM, quit_cb, app);
}

static void
dictionary_application_activate (GApplication *app)
{
  DictionaryApplication *self = DICTIONARY_APPLICATION (app);
  gboolean first_activation = (gtk_application_get_active_window (GTK_APPLICATION (app)) == NULL);

  ensure_services_started (self);

  /* A process launched with --background (and no --look-up term, and no
     auto-paste request) stays headless on this one, very first
     activation: the tray icon and/or clipboard watcher set up above are
     already doing the actual work. Any later activation — a second
     `dictionary` invocation, or "Open Dictionary" from the tray — goes
     through present_window_plain() as normal. */
  if (self->background_requested) {
    /* Consume the flag: it must only suppress the window on the one
       activation that came from the --background launch itself. Before
       this, it stayed set forever, so with the window closed every later
       plain `dictionary` launch was treated as headless too and did
       nothing visible. */
    self->background_requested = FALSE;
    if (first_activation &&
        (self->lookup_term == NULL || self->lookup_term[0] == '\0') &&
        !self->auto_paste_requested) {
      log_warning ("Starting in the background (tray/clipboard service only)");
      return;
    }
  }

  if (first_activation) {
    DictionaryWindow *win = dictionary_window_new (GTK_APPLICATION (app), _("Dictionary"),
                                               self->lookup_term, self->auto_paste_requested);
    if (self->development_mode)
      gtk_widget_add_css_class (GTK_WIDGET (win), "devel");
    g_set_weak_pointer (&self->win, win);
    gtk_window_present (GTK_WINDOW (win));
  } else {
    present_window_plain (self);
  }
}

static int
dictionary_application_command_line (GApplication *app, GApplicationCommandLine *cmd)
{
  DictionaryApplication *self = DICTIONARY_APPLICATION (app);
  GVariantDict *opts = g_application_command_line_get_options_dict (cmd);
  const char *term = "";

  if (g_variant_dict_contains (opts, "info")) {
    wb_get_version_info (self->version);
    return 0;
  }

  if (g_variant_dict_lookup (opts, "look-up", "&s", &term) == FALSE)
    term = "";

  if (g_variant_dict_contains (opts, "auto-paste"))
    self->auto_paste_requested = TRUE;

  if (g_variant_dict_contains (opts, "background")) {
    self->background_requested = TRUE;
    /* Launching headless only makes sense if the process is allowed to
       keep running with no window open — turn that preference on so the
       GApplication hold applied in ensure_services_started() actually
       keeps it alive. It's a real, persisted preference change (the same
       one the tray menu and Preferences toggle), not a one-off flag, so
       a plain `dictionary` launch afterwards keeps behaving the same way
       until the person turns it back off. */
    wb_settings_set_run_in_background (wb_settings_get (), TRUE);
  }

  wb_log_init (self->development_mode || g_variant_dict_contains (opts, "verbose"));

  if (self->win != NULL) {
    if (term[0] != '\0')
      dictionary_window_trigger_search (self->win, term);
    else if (self->auto_paste_requested)
      dictionary_window_queue_auto_paste (self->win);
  } else {
    g_free (self->lookup_term);
    self->lookup_term = g_strdup (term);
  }

  g_application_activate (app);
  return 0;
}

static void
dictionary_application_shutdown (GApplication *app)
{
  DictionaryApplication *self = DICTIONARY_APPLICATION (app);
  if (self->win != NULL)
    dictionary_window_save_state (self->win);

  wb_clipboard_watch_set_state_callback (NULL, NULL);
  wb_clipboard_watch_stop ();
  g_clear_pointer (&self->tray, wb_tray_free);

  G_APPLICATION_CLASS (dictionary_application_parent_class)->shutdown (app);
}

static void
dictionary_application_finalize (GObject *object)
{
  DictionaryApplication *self = DICTIONARY_APPLICATION (object);
  g_clear_weak_pointer (&self->win);
  g_clear_pointer (&self->tray, wb_tray_free); /* safety net; shutdown already does this */
  g_free (self->app_id);
  g_free (self->version);
  g_free (self->lookup_term);
  G_OBJECT_CLASS (dictionary_application_parent_class)->finalize (object);
}

static void
dictionary_application_class_init (DictionaryApplicationClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GApplicationClass *app_class = G_APPLICATION_CLASS (klass);

  object_class->finalize = dictionary_application_finalize;
  app_class->startup = dictionary_application_startup;
  app_class->activate = dictionary_application_activate;
  app_class->command_line = dictionary_application_command_line;
  app_class->shutdown = dictionary_application_shutdown;
}

static void
dictionary_application_init (DictionaryApplication *self)
{
  self->version = g_strdup ("0.0.0");
}

static DictionaryApplication *
dictionary_application_new (const char *app_id, const char *version)
{
  DictionaryApplication *self = g_object_new (DICTIONARY_TYPE_APPLICATION,
                                            "application-id", app_id,
                                            "flags", G_APPLICATION_HANDLES_COMMAND_LINE,
                                            NULL);
  g_free (self->app_id);
  g_free (self->version);
  self->app_id = g_strdup (app_id);
  self->version = g_strdup (version);

  g_set_application_name (_("Dictionary"));
  g_set_prgname (app_id);

  g_application_add_main_option (G_APPLICATION (self), "look-up", 'l', G_OPTION_FLAG_NONE,
                                 G_OPTION_ARG_STRING, "Term to look up", NULL);
  g_application_add_main_option (G_APPLICATION (self), "info", 'i', G_OPTION_FLAG_NONE,
                                 G_OPTION_ARG_NONE, "Print version info", NULL);
  g_application_add_main_option (G_APPLICATION (self), "verbose", 'v', G_OPTION_FLAG_NONE,
                                 G_OPTION_ARG_NONE, "Make it scream louder", NULL);
  g_application_add_main_option (G_APPLICATION (self), "auto-paste", 'p', G_OPTION_FLAG_NONE,
                                 G_OPTION_ARG_NONE, "Automatically paste and search clipboard content", NULL);
  g_application_add_main_option (G_APPLICATION (self), "background", 'b', G_OPTION_FLAG_NONE,
                                 G_OPTION_ARG_NONE,
                                 "Start without opening the main window (tray icon / clipboard "
                                 "lookup service only); also turns on \"Keep Running in Background\"",
                                 NULL);

  wb_create_required_dirs ();
  return self;
}

int
main (int argc, char **argv)
{
  setlocale (LC_ALL, "");
#if defined(GETTEXT_PACKAGE) && defined(LOCALEDIR)
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);
#endif

  g_autoptr (DictionaryApplication) app = dictionary_application_new (APP_ID, APP_VERSION);
  return g_application_run (G_APPLICATION (app), argc, argv);
}
