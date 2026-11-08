#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define DICTIONARY_TYPE_WINDOW (dictionary_window_get_type ())

G_DECLARE_FINAL_TYPE (DictionaryWindow, dictionary_window, DICTIONARY, WINDOW, AdwApplicationWindow)

DictionaryWindow *dictionary_window_new (GtkApplication *app,
                                     const char     *title,
                                     const char     *term,
                                     gboolean        auto_paste_requested);

void dictionary_window_trigger_search (DictionaryWindow *self, const char *text);

void dictionary_window_queue_auto_paste (DictionaryWindow *self);

void dictionary_window_save_state (DictionaryWindow *self);

void dictionary_window_set_completion_enabled (DictionaryWindow *self, gboolean enabled);

void dictionary_window_refresh_current_search_pronunciations (DictionaryWindow *self);

GtkWidget *dictionary_window_get_search_button (DictionaryWindow *self);

G_END_DECLS
