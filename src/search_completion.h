#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct _WbSearchCompletion WbSearchCompletion;

typedef char **(*WbCompletionItemProvider) (const char *text, int limit, gpointer user_data);
typedef void   (*WbCompletionActivate)     (const char *item, gpointer user_data);

WbSearchCompletion *wb_search_completion_new (GtkWidget *parent,
                                              GtkEntry  *entry,
                                              WbCompletionItemProvider item_provider,
                                              WbCompletionActivate     activate,
                                              gpointer user_data);

WbSearchCompletion *wb_search_completion_ref   (WbSearchCompletion *self);
void                wb_search_completion_unref (WbSearchCompletion *self);

void     wb_search_completion_set_enabled (WbSearchCompletion *self, gboolean enabled);
void     wb_search_completion_update      (WbSearchCompletion *self, const char *text);
void     wb_search_completion_clear       (WbSearchCompletion *self);
void     wb_search_completion_hide        (WbSearchCompletion *self);
gboolean wb_search_completion_handle_key_pressed (WbSearchCompletion *self, guint keyval);
gboolean wb_search_completion_entry_has_focus    (WbSearchCompletion *self);

G_END_DECLS
