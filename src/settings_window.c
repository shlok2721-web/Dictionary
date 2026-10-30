#include "settings_window.h"

#include "constants.h"
#include "settings.h"

struct _SettingsDialog {
  AdwPreferencesDialog parent_instance;

  AdwSwitchRow *dark_ui_switch;
  AdwSwitchRow *double_click_switch;
  AdwSwitchRow *live_search_switch;
  AdwSwitchRow *auto_paste_switch;
  AdwComboRow  *pronunciations_accent_row;

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
load_settings (SettingsDialog *self)
{
  WbSettings *s = wb_settings_get ();
  adw_switch_row_set_active (self->double_click_switch, wb_settings_get_double_click (s));
  adw_switch_row_set_active (self->live_search_switch, wb_settings_get_live_search (s));
  adw_switch_row_set_active (self->auto_paste_switch, wb_settings_get_auto_paste_on_launch (s));
  adw_combo_row_set_selected (self->pronunciations_accent_row,
                              (guint) wb_accent_index (wb_settings_get_pronunciations_accent (s)));
  adw_switch_row_set_active (self->dark_ui_switch, wb_settings_get_gtk_dark_ui (s));
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
  return self;
}
