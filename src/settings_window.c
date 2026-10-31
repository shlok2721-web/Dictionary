#include "settings_window.h"

#include "clipboard_watch.h"
#include "constants.h"
#include "popup_window.h"
#include "settings.h"

struct _SettingsDialog {
  AdwPreferencesDialog parent_instance;

  AdwSwitchRow *dark_ui_switch;
  AdwSwitchRow *double_click_switch;
  AdwSwitchRow *live_search_switch;
  AdwSwitchRow *auto_paste_switch;
  AdwComboRow  *pronunciations_accent_row;

  AdwSwitchRow *tray_enabled_switch;
  AdwSwitchRow *run_in_background_switch;
  AdwSwitchRow *clipboard_watch_switch;
  AdwComboRow  *lookup_source_row;
  AdwSwitchRow *auto_show_popup_switch;
  AdwSpinRow   *popup_size_row;

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
load_settings (SettingsDialog *self)
{
  WbSettings *s = wb_settings_get ();
  adw_switch_row_set_active (self->double_click_switch, wb_settings_get_double_click (s));
  adw_switch_row_set_active (self->live_search_switch, wb_settings_get_live_search (s));
  adw_switch_row_set_active (self->auto_paste_switch, wb_settings_get_auto_paste_on_launch (s));
  adw_combo_row_set_selected (self->pronunciations_accent_row,
                              (guint) wb_accent_index (wb_settings_get_pronunciations_accent (s)));
  adw_switch_row_set_active (self->dark_ui_switch, wb_settings_get_gtk_dark_ui (s));

  adw_switch_row_set_active (self->tray_enabled_switch, wb_settings_get_tray_enabled (s));
  adw_switch_row_set_active (self->run_in_background_switch, wb_settings_get_run_in_background (s));
  adw_switch_row_set_active (self->clipboard_watch_switch, wb_settings_get_clipboard_watch_enabled (s));
  adw_combo_row_set_selected (self->lookup_source_row, (guint) wb_settings_get_lookup_source (s));
  adw_switch_row_set_active (self->auto_show_popup_switch, wb_settings_get_auto_show_popup (s));
  adw_spin_row_set_value (self->popup_size_row, wb_settings_get_popup_size_percent (s));
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
  gtk_widget_class_bind_template_child (widget_class, SettingsDialog, tray_enabled_switch);
  gtk_widget_class_bind_template_child (widget_class, SettingsDialog, run_in_background_switch);
  gtk_widget_class_bind_template_child (widget_class, SettingsDialog, clipboard_watch_switch);
  gtk_widget_class_bind_template_child (widget_class, SettingsDialog, lookup_source_row);
  gtk_widget_class_bind_template_child (widget_class, SettingsDialog, auto_show_popup_switch);
  gtk_widget_class_bind_template_child (widget_class, SettingsDialog, popup_size_row);
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
  g_signal_connect (self->tray_enabled_switch, "notify::active", G_CALLBACK (on_tray_enabled_switch), self);
  g_signal_connect (self->run_in_background_switch, "notify::active", G_CALLBACK (on_run_in_background_switch), self);
  g_signal_connect (self->clipboard_watch_switch, "notify::active", G_CALLBACK (on_clipboard_watch_switch), self);
  g_signal_connect (self->lookup_source_row, "notify::selected", G_CALLBACK (on_lookup_source_selected), self);
  g_signal_connect (self->auto_show_popup_switch, "notify::active", G_CALLBACK (on_auto_show_popup_switch), self);
  g_signal_connect (self->popup_size_row, "notify::value", G_CALLBACK (on_popup_size_changed), self);
  return self;
}
