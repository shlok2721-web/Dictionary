#include <adwaita.h>
#include <glib/gi18n.h>
#include <glib-unix.h>
#include <locale.h>

#include "base.h"
#include "constants.h"
#include "settings.h"
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
  GtkWindow *active = gtk_application_get_active_window (GTK_APPLICATION (app));

  if (active == NULL) {
    DictionaryWindow *win = dictionary_window_new (GTK_APPLICATION (app), _("Dictionary"),
                                               self->lookup_term, self->auto_paste_requested);
    if (self->development_mode)
      gtk_widget_add_css_class (GTK_WIDGET (win), "devel");
    g_set_weak_pointer (&self->win, win);
    setup_actions (self);
    active = GTK_WINDOW (win);
  } else {
    g_set_weak_pointer (&self->win, DICTIONARY_WINDOW (active));
  }
  gtk_window_present (active);
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
  G_APPLICATION_CLASS (dictionary_application_parent_class)->shutdown (app);
}

static void
dictionary_application_finalize (GObject *object)
{
  DictionaryApplication *self = DICTIONARY_APPLICATION (object);
  g_clear_weak_pointer (&self->win);
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
