#pragma once

#include <adwaita.h>
#include "window.h"

G_BEGIN_DECLS

#define SETTINGS_TYPE_DIALOG (settings_dialog_get_type ())
G_DECLARE_FINAL_TYPE (SettingsDialog, settings_dialog, SETTINGS, DIALOG, AdwPreferencesDialog)

SettingsDialog *settings_dialog_new (DictionaryWindow *parent);

/* Let the person pick a database file.  Opens a file chooser, validates the
 * choice (showing an error dialog on PARENT if it is unusable) and, if it is
 * good, stores it in Preferences.  CB is called exactly once: PICKED is TRUE
 * only when a valid file was saved.  PARENT is kept alive until CB returns. */
typedef void (*SettingsDatabasePickedFunc) (gboolean picked, gpointer user_data);
void settings_pick_database_async (GtkWidget                  *parent,
                                   SettingsDatabasePickedFunc  cb,
                                   gpointer                    user_data);

G_END_DECLS
