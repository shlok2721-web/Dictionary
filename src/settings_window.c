#include "settings_window.h"

#include <glib/gi18n.h>
#include <string.h>

#include "clipboard_watch.h"
#include "constants.h"
#include "database.h"
#include "popup_window.h"
#include "settings.h"

struct _SettingsDialog {
  AdwPreferencesDialog parent_instance;

  AdwSwitchRow *dark_ui_switch;
  AdwSwitchRow *double_click_switch;
  AdwSwitchRow *live_search_switch;
  AdwSwitchRow *auto_paste_switch;
  AdwComboRow  *pronunciations_accent_row;
  AdwComboRow  *voice_style_row;
  AdwSpinRow   *voice_pitch_row;
  AdwSwitchRow *use_recorded_audio_switch;

  AdwSwitchRow *tray_enabled_switch;
  AdwSwitchRow *run_in_background_switch;
  AdwSwitchRow *clipboard_watch_switch;
  AdwComboRow  *lookup_source_row;
  AdwSwitchRow *auto_show_popup_switch;
  AdwSpinRow   *popup_size_row;
  AdwSpinRow   *popup_autohide_row;

  AdwActionRow *database_row;
  GtkButton    *database_choose_button;
  GtkButton    *database_reset_button;

  DictionaryWindow *parent;
};

G_DEFINE_FINAL_TYPE (SettingsDialog, settings_dialog, ADW_TYPE_PREFERENCES_DIALOG)

static void
on_double_click_switch (GObject *sw, GParamSpec *pspec, SettingsDialog *self)
{
  wb_settings_set_double_click (wb_settings_get (), adw_switch_row_get_active (ADW_SWITCH_ROW (sw)));
}

static void
on_live_search_switch (GObject *sw, GParamSpec *pspec, SettingsDialog *self)
{
  gboolean active = adw_switch_row_get_active (ADW_SWITCH_ROW (sw));

  if (self->parent != NULL) {
    GtkWidget *search_button = dictionary_window_get_search_button (self->parent);
    dictionary_window_set_completion_enabled (self->parent, !active);
    gtk_widget_set_visible (search_button, !active);
    if (!active)
      gtk_window_set_default_widget (GTK_WINDOW (self->parent), search_button);
  }
  wb_settings_set_live_search (wb_settings_get (), active);
}

static void
on_auto_paste_switch (GObject *sw, GParamSpec *pspec, SettingsDialog *self)
{
  wb_settings_set_auto_paste_on_launch (wb_settings_get (), adw_switch_row_get_active (ADW_SWITCH_ROW (sw)));
}

static void
on_accent_selected (GObject *row, GParamSpec *pspec, SettingsDialog *self)
{
  WbSettings *settings = wb_settings_get ();
  WbPronunciationAccent previous = wb_settings_get_pronunciations_accent (settings);
  WbPronunciationAccent next =
    wb_accent_from_index ((int) adw_combo_row_get_selected (ADW_COMBO_ROW (row)));

  if (next == previous)
    return;

  wb_settings_set_pronunciations_accent (settings, next);
  if (self->parent != NULL)
    dictionary_window_refresh_current_search_pronunciations (self->parent);
}

static void
on_voice_style_selected (GObject *row, GParamSpec *pspec, SettingsDialog *self)
{
  WbVoiceStyle next = wb_voice_from_index ((int) adw_combo_row_get_selected (ADW_COMBO_ROW (row)));
  wb_settings_set_voice_style (wb_settings_get (), next);
}

static void
on_voice_pitch_changed (GObject *row, GParamSpec *pspec, SettingsDialog *self)
{
  wb_settings_set_voice_pitch (wb_settings_get (),
                               (int) adw_spin_row_get_value (ADW_SPIN_ROW (row)));
}

static void
on_use_recorded_audio_switch (GObject *sw, GParamSpec *pspec, SettingsDialog *self)
{
  wb_settings_set_use_recorded_audio (wb_settings_get (), adw_switch_row_get_active (ADW_SWITCH_ROW (sw)));
}

static void
on_dark_ui_switch (GObject *sw, GParamSpec *pspec, SettingsDialog *self)
{
  gboolean active = adw_switch_row_get_active (ADW_SWITCH_ROW (sw));
  wb_settings_set_gtk_dark_ui (wb_settings_get (), active);
  adw_style_manager_set_color_scheme (adw_style_manager_get_default (),
                                      active ? ADW_COLOR_SCHEME_FORCE_DARK : ADW_COLOR_SCHEME_DEFAULT);
}

static void
on_tray_enabled_switch (GObject *sw, GParamSpec *pspec, SettingsDialog *self)
{
  /* The tray icon itself is only created/torn down once, at startup and
     shutdown (see ensure_services_started() in main.c) — there's no live
     D-Bus re-registration dance here, just the preference that decides
     whether it happens next time Dictionary starts, which the subtitle
     in the UI already says plainly. */
  wb_settings_set_tray_enabled (wb_settings_get (), adw_switch_row_get_active (ADW_SWITCH_ROW (sw)));
}

static void
on_run_in_background_switch (GObject *sw, GParamSpec *pspec, SettingsDialog *self)
{
  gboolean active = adw_switch_row_get_active (ADW_SWITCH_ROW (sw));
  WbSettings *s = wb_settings_get ();
  gboolean previous = wb_settings_get_run_in_background (s);

  wb_settings_set_run_in_background (s, active);

  /* Keep the process's GApplication hold count in lock-step with the
     setting, exactly like the tray menu's equivalent toggle does — both
     paths change the same persisted value, so both must balance the same
     counter or the app could end up either unkillable or quitting the
     instant the window closes despite the switch being on. */
  if (self->parent != NULL) {
    GApplication *app = G_APPLICATION (gtk_window_get_application (GTK_WINDOW (self->parent)));
    if (app != NULL) {
      if (active && !previous)
        g_application_hold (app);
      else if (!active && previous)
        g_application_release (app);
    }
  }
}

static void
on_clipboard_watch_switch (GObject *sw, GParamSpec *pspec, SettingsDialog *self)
{
  gboolean active = adw_switch_row_get_active (ADW_SWITCH_ROW (sw));
  wb_settings_set_clipboard_watch_enabled (wb_settings_get (), active);
  if (active)
    wb_clipboard_watch_start (wb_popup_window_handle_clipboard_term, NULL);
  else
    wb_clipboard_watch_stop ();
}

static void
on_lookup_source_selected (GObject *row, GParamSpec *pspec, SettingsDialog *self)
{
  WbLookupSource next = (WbLookupSource) adw_combo_row_get_selected (ADW_COMBO_ROW (row));
  if (next == wb_settings_get_lookup_source (wb_settings_get ()))
    return;
  wb_settings_set_lookup_source (wb_settings_get (), next);

  /* Re-arm the watcher so the new trigger takes effect immediately. */
  if (wb_clipboard_watch_is_active ()) {
    wb_clipboard_watch_stop ();
    wb_clipboard_watch_start (wb_popup_window_handle_clipboard_term, NULL);
  }
}

static void
on_auto_show_popup_switch (GObject *sw, GParamSpec *pspec, SettingsDialog *self)
{
  wb_settings_set_auto_show_popup (wb_settings_get (), adw_switch_row_get_active (ADW_SWITCH_ROW (sw)));
}

static void
on_popup_size_changed (GObject *row, GParamSpec *pspec, SettingsDialog *self)
{
  wb_settings_set_popup_size_percent (wb_settings_get (),
                                      (int) adw_spin_row_get_value (ADW_SPIN_ROW (row)));
}

static void
on_popup_autohide_changed (GObject *row, GParamSpec *pspec, SettingsDialog *self)
{
  wb_settings_set_popup_autohide_seconds (wb_settings_get (),
                                          (int) adw_spin_row_get_value (ADW_SPIN_ROW (row)));
}

/* ---------------------------------------------------------------------- */
/* Database location                                                      */
/* ---------------------------------------------------------------------- */

static void
refresh_database_row (SettingsDialog *self)
{
  g_autofree char *custom = wb_settings_get_database_path (wb_settings_get ());
  const char *active = wb_database_path ();   /* what this session is using */
  g_autofree char *subtitle = NULL;

  if (custom != NULL) {
    if (!g_file_test (custom, G_FILE_TEST_IS_REGULAR))
      subtitle = g_strdup_printf (_("%s\nFile not found — the default database is used instead"), custom);
    else if (active != NULL && strcmp (custom, active) != 0)
      subtitle = g_strdup_printf (_("%s\nWill be used after restarting Dictionary"), custom);
    else
      subtitle = g_strdup (custom);
  } else if (active != NULL) {
    subtitle = g_strdup_printf (_("Automatic: %s"), active);
  } else {
    subtitle = g_strdup (_("Automatic — no database found"));
  }

  adw_action_row_set_subtitle (self->database_row, subtitle);
  gtk_widget_set_sensitive (GTK_WIDGET (self->database_reset_button), custom != NULL);
}

static void
announce_restart_needed (SettingsDialog *self)
{
  adw_preferences_dialog_add_toast (ADW_PREFERENCES_DIALOG (self),
                                    adw_toast_new (_("Restart Dictionary to use this database")));
}

static void
show_database_error (GtkWidget *parent, const char *message)
{
  AdwDialog *alert = adw_alert_dialog_new (_("Can't Use This File"), message);
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (alert), "ok", _("_OK"));
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (alert), "ok");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (alert), "ok");
  adw_dialog_present (alert, parent);
}

typedef struct {
  GtkWidget                  *parent;   /* strong ref */
  SettingsDatabasePickedFunc  cb;
  gpointer                    user_data;
} PickData;

static void
pick_finish (PickData *pd, gboolean picked)
{
  if (pd->cb != NULL)
    pd->cb (picked, pd->user_data);
  g_object_unref (pd->parent);
  g_free (pd);
}

static void
on_database_file_chosen (GObject *source, GAsyncResult *res, gpointer user_data)
{
  PickData *pd = user_data;
  g_autoptr (GError) err = NULL;
  g_autoptr (GFile) file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), res, &err);

  if (file == NULL) {
    /* Closing the chooser without picking anything is not an error. */
    if (!g_error_matches (err, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED) &&
        !g_error_matches (err, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED) &&
        !g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      show_database_error (pd->parent, err ? err->message : _("The file couldn't be selected."));
    pick_finish (pd, FALSE);
    return;
  }

  g_autofree char *path = g_file_get_path (file);
  g_autofree char *problem = NULL;
  gboolean picked = FALSE;

  if (path == NULL) {
    show_database_error (pd->parent, _("Only files on this computer can be used. Copy the database to a local folder first."));
  } else if (!wb_database_validate (path, &problem)) {
    show_database_error (pd->parent, problem);
  } else {
    wb_settings_set_database_path (wb_settings_get (), path);
    picked = TRUE;
  }
  pick_finish (pd, picked);
}

void
settings_pick_database_async (GtkWidget *parent, SettingsDatabasePickedFunc cb, gpointer user_data)
{
  g_return_if_fail (GTK_IS_WIDGET (parent));

  g_autoptr (GtkFileDialog) dialog = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dialog, _("Choose Dictionary Database"));
  gtk_file_dialog_set_accept_label (dialog, _("_Select"));

  g_autoptr (GListStore) filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
  g_autoptr (GtkFileFilter) db_filter = gtk_file_filter_new ();
  gtk_file_filter_set_name (db_filter, _("SQLite databases"));
  gtk_file_filter_add_pattern (db_filter, "*.db");
  gtk_file_filter_add_pattern (db_filter, "*.sqlite");
  gtk_file_filter_add_pattern (db_filter, "*.sqlite3");
  g_list_store_append (filters, db_filter);
  g_autoptr (GtkFileFilter) all_filter = gtk_file_filter_new ();
  gtk_file_filter_set_name (all_filter, _("All files"));
  gtk_file_filter_add_pattern (all_filter, "*");
  g_list_store_append (filters, all_filter);
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  gtk_file_dialog_set_default_filter (dialog, db_filter);

  /* Start in the folder of the current choice (or the active database). */
  g_autofree char *custom = wb_settings_get_database_path (wb_settings_get ());
  const char *start = custom != NULL ? custom : wb_database_path ();
  if (start != NULL) {
    g_autoptr (GFile) f = g_file_new_for_path (start);
    gtk_file_dialog_set_initial_file (dialog, f);
  }

  PickData *pd = g_new0 (PickData, 1);
  pd->parent = g_object_ref (parent);
  pd->cb = cb;
  pd->user_data = user_data;

  GtkRoot *root = gtk_widget_get_root (parent);
  gtk_file_dialog_open (dialog, root ? GTK_WINDOW (root) : NULL, NULL,
                        on_database_file_chosen, pd);
}

static void
on_database_picked (gboolean picked, gpointer user_data)
{
  SettingsDialog *self = SETTINGS_DIALOG (user_data);
  if (!picked)
    return;
  refresh_database_row (self);
  announce_restart_needed (self);
}

static void
on_database_choose_clicked (GtkButton *button, SettingsDialog *self)
{
  settings_pick_database_async (GTK_WIDGET (self), on_database_picked, self);
}

static void
on_database_reset_clicked (GtkButton *button, SettingsDialog *self)
{
  wb_settings_set_database_path (wb_settings_get (), NULL);
  refresh_database_row (self);
  announce_restart_needed (self);
}

static void
load_settings (SettingsDialog *self)
{
  WbSettings *s = wb_settings_get ();
  adw_switch_row_set_active (self->double_click_switch, wb_settings_get_double_click (s));
  adw_switch_row_set_active (self->live_search_switch, wb_settings_get_live_search (s));
  adw_switch_row_set_active (self->auto_paste_switch, wb_settings_get_auto_paste_on_launch (s));
  adw_combo_row_set_selected (self->pronunciations_accent_row,
                              (guint) wb_accent_index (wb_settings_get_pronunciations_accent (s)));
  adw_combo_row_set_selected (self->voice_style_row,
                              (guint) wb_voice_index (wb_settings_get_voice_style (s)));
  adw_spin_row_set_value (self->voice_pitch_row, wb_settings_get_voice_pitch (s));
  adw_switch_row_set_active (self->use_recorded_audio_switch, wb_settings_get_use_recorded_audio (s));
  adw_switch_row_set_active (self->dark_ui_switch, wb_settings_get_gtk_dark_ui (s));

  adw_switch_row_set_active (self->tray_enabled_switch, wb_settings_get_tray_enabled (s));
  adw_switch_row_set_active (self->run_in_background_switch, wb_settings_get_run_in_background (s));
  adw_switch_row_set_active (self->clipboard_watch_switch, wb_settings_get_clipboard_watch_enabled (s));
  adw_combo_row_set_selected (self->lookup_source_row, (guint) wb_settings_get_lookup_source (s));
  adw_switch_row_set_active (self->auto_show_popup_switch, wb_settings_get_auto_show_popup (s));
  adw_spin_row_set_value (self->popup_size_row, wb_settings_get_popup_size_percent (s));
  adw_spin_row_set_value (self->popup_autohide_row, wb_settings_get_popup_autohide_seconds (s));

  refresh_database_row (self);
}

static void
settings_dialog_dispose (GObject *object)
{
  SettingsDialog *self = SETTINGS_DIALOG (object);
  g_clear_weak_pointer (&self->parent);
  gtk_widget_dispose_template (GTK_WIDGET (self), SETTINGS_TYPE_DIALOG);
  G_OBJECT_CLASS (settings_dialog_parent_class)->dispose (object);
}

static void
settings_dialog_class_init (SettingsDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = settings_dialog_dispose;

  gtk_widget_class_set_template_from_resource (widget_class, WB_RES_PATH "/ui/settings-window.ui");
  gtk_widget_class_bind_template_child (widget_class, SettingsDialog, dark_ui_switch);
  gtk_widget_class_bind_template_child (widget_class, SettingsDialog, double_click_switch);
  gtk_widget_class_bind_template_child (widget_class, SettingsDialog, live_search_switch);
  gtk_widget_class_bind_template_child (widget_class, SettingsDialog, auto_paste_switch);
  gtk_widget_class_bind_template_child (widget_class, SettingsDialog, pronunciations_accent_row);
  gtk_widget_class_bind_template_child (widget_class, SettingsDialog, voice_style_row);
  gtk_widget_class_bind_template_child (widget_class, SettingsDialog, voice_pitch_row);
  gtk_widget_class_bind_template_child (widget_class, SettingsDialog, use_recorded_audio_switch);
  gtk_widget_class_bind_template_child (widget_class, SettingsDialog, tray_enabled_switch);
  gtk_widget_class_bind_template_child (widget_class, SettingsDialog, run_in_background_switch);
  gtk_widget_class_bind_template_child (widget_class, SettingsDialog, clipboard_watch_switch);
  gtk_widget_class_bind_template_child (widget_class, SettingsDialog, lookup_source_row);
  gtk_widget_class_bind_template_child (widget_class, SettingsDialog, auto_show_popup_switch);
  gtk_widget_class_bind_template_child (widget_class, SettingsDialog, popup_size_row);
  gtk_widget_class_bind_template_child (widget_class, SettingsDialog, popup_autohide_row);
  gtk_widget_class_bind_template_child (widget_class, SettingsDialog, database_row);
  gtk_widget_class_bind_template_child (widget_class, SettingsDialog, database_choose_button);
  gtk_widget_class_bind_template_child (widget_class, SettingsDialog, database_reset_button);
}

static void
settings_dialog_init (SettingsDialog *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));
}

SettingsDialog *
settings_dialog_new (DictionaryWindow *parent)
{
  SettingsDialog *self = g_object_new (SETTINGS_TYPE_DIALOG, NULL);
  g_set_weak_pointer (&self->parent, parent);

  load_settings (self);

  g_signal_connect (self->double_click_switch, "notify::active", G_CALLBACK (on_double_click_switch), self);
  g_signal_connect (self->live_search_switch, "notify::active", G_CALLBACK (on_live_search_switch), self);
  g_signal_connect (self->auto_paste_switch, "notify::active", G_CALLBACK (on_auto_paste_switch), self);
  g_signal_connect (self->dark_ui_switch, "notify::active", G_CALLBACK (on_dark_ui_switch), self);
  g_signal_connect (self->pronunciations_accent_row, "notify::selected", G_CALLBACK (on_accent_selected), self);
  g_signal_connect (self->voice_style_row, "notify::selected", G_CALLBACK (on_voice_style_selected), self);
  g_signal_connect (self->voice_pitch_row, "notify::value", G_CALLBACK (on_voice_pitch_changed), self);
  g_signal_connect (self->use_recorded_audio_switch, "notify::active", G_CALLBACK (on_use_recorded_audio_switch), self);
  g_signal_connect (self->tray_enabled_switch, "notify::active", G_CALLBACK (on_tray_enabled_switch), self);
  g_signal_connect (self->run_in_background_switch, "notify::active", G_CALLBACK (on_run_in_background_switch), self);
  g_signal_connect (self->clipboard_watch_switch, "notify::active", G_CALLBACK (on_clipboard_watch_switch), self);
  g_signal_connect (self->lookup_source_row, "notify::selected", G_CALLBACK (on_lookup_source_selected), self);
  g_signal_connect (self->auto_show_popup_switch, "notify::active", G_CALLBACK (on_auto_show_popup_switch), self);
  g_signal_connect (self->popup_size_row, "notify::value", G_CALLBACK (on_popup_size_changed), self);
  g_signal_connect (self->popup_autohide_row, "notify::value", G_CALLBACK (on_popup_autohide_changed), self);
  g_signal_connect (self->database_choose_button, "clicked", G_CALLBACK (on_database_choose_clicked), self);
  g_signal_connect (self->database_reset_button, "clicked", G_CALLBACK (on_database_reset_clicked), self);
  return self;
}
