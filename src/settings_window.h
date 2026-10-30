#pragma once

#include <adwaita.h>
#include "window.h"

G_BEGIN_DECLS

#define SETTINGS_TYPE_DIALOG (settings_dialog_get_type ())
G_DECLARE_FINAL_TYPE (SettingsDialog, settings_dialog, SETTINGS, DIALOG, AdwPreferencesDialog)

SettingsDialog *settings_dialog_new (DictionaryWindow *parent);

G_END_DECLS
