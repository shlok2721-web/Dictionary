#include "search_completion.h"

#include <math.h>
#include <string.h>

#include "utils.h"

#define ENTRY_CLASS        "completion-entry"
#define ENTRY_ACTIVE_CLASS "completion-active"
#define MAX_ITEMS          10

struct _WbSearchCompletion {
  gint      ref_count;
  gboolean  torn_down;

  GtkWidget *parent;
  GtkEntry  *entry;
  WbCompletionItemProvider item_provider;
  WbCompletionActivate     activate;
  gpointer  user_data;

  gboolean  enabled;
  char    **items;
  guint     n_items;

  GtkWidget *listbox;
  GtkWidget *scroller;
  GtkWidget *popover;

  GtkEventController *key_controller, *focus_controller, *click_controller;
  gulong parent_destroy_id;
};

static gboolean
contains_widget (GtkWidget *ancestor, GtkWidget *widget)
{
  return widget == ancestor || (widget != NULL && gtk_widget_is_ancestor (widget, ancestor));
}

static void
set_entry_active (WbSearchCompletion *self, gboolean active)
{
  if (self->entry == NULL)
    return;
  if (active)
    gtk_widget_add_css_class (GTK_WIDGET (self->entry), ENTRY_ACTIVE_CLASS);
  else
    gtk_widget_remove_css_class (GTK_WIDGET (self->entry), ENTRY_ACTIVE_CLASS);
}

static void
free_items (WbSearchCompletion *self)
{
  g_strfreev (self->items);
  self->items = NULL;
  self->n_items = 0;
}

static void
listbox_remove_all (WbSearchCompletion *self)
{
  gtk_list_box_remove_all (GTK_LIST_BOX (self->listbox));
}

static int
selected_index (WbSearchCompletion *self, gboolean fallback_to_first)
{
  GtkListBoxRow *row = gtk_list_box_get_selected_row (GTK_LIST_BOX (self->listbox));
  if (row != NULL) {
    int index = gtk_list_box_row_get_index (row);
    if (index >= 0 && (guint) index < self->n_items)
      return index;
  }
  if (fallback_to_first && self->n_items > 0)
    return 0;
  return -1;
}

static GtkWidget *
create_row (const char *item)
{
  GtkWidget *row = gtk_list_box_row_new ();
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
  gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (row), TRUE);
  gtk_widget_set_focusable (row, FALSE);

  GtkWidget *label = gtk_label_new (item);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_margin_top (label, 3);
  gtk_widget_set_margin_bottom (label, 3);
  gtk_widget_set_margin_start (label, 10);
  gtk_widget_set_margin_end (label, 10);
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), label);
  return row;
}

gboolean
wb_search_completion_entry_has_focus (WbSearchCompletion *self)
{
  if (self->entry == NULL)
    return FALSE;
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self->entry));
  GtkWidget *focus = root ? gtk_root_get_focus (root) : NULL;
  return contains_widget (GTK_WIDGET (self->entry), focus);
}

void
wb_search_completion_hide (WbSearchCompletion *self)
{
  if (self->torn_down || self->popover == NULL)
    return;
  gtk_list_box_unselect_all (GTK_LIST_BOX (self->listbox));
  set_entry_active (self, FALSE);
  if (gtk_widget_get_visible (self->popover))
    gtk_popover_popdown (GTK_POPOVER (self->popover));
}

void
wb_search_completion_clear (WbSearchCompletion *self)
{
  if (self->torn_down)
    return;
  free_items (self);
  listbox_remove_all (self);
  wb_search_completion_hide (self);
}

static gboolean
sync_geometry (WbSearchCompletion *self)
{
  graphene_rect_t b;
  if (!gtk_widget_compute_bounds (GTK_WIDGET (self->entry), self->parent, &b))
    return FALSE;

  int entry_width = (int) lroundf (graphene_rect_get_width (&b));
  entry_width = MAX (entry_width, gtk_widget_get_width (GTK_WIDGET (self->entry)));
  entry_width = MAX (entry_width, 1);

  GdkRectangle rect = {
    .x = (int) lroundf (graphene_rect_get_x (&b)),
    .y = (int) lroundf (graphene_rect_get_y (&b) + graphene_rect_get_height (&b)) - 1,
    .width = entry_width,
    .height = 1,
  };
  gtk_popover_set_offset (GTK_POPOVER (self->popover), 0, 0);
  gtk_popover_set_pointing_to (GTK_POPOVER (self->popover), &rect);
  gtk_widget_set_size_request (self->scroller, entry_width, -1);
  return TRUE;
}

static void
grab_focus_without_selecting (GtkEntry *entry)
{
  gtk_entry_grab_focus_without_selecting (entry);
}

void
wb_search_completion_update (WbSearchCompletion *self, const char *text)
{
  if (self->torn_down)
    return;
  if (!self->enabled || text == NULL || wb_str_is_blank (text) ||
      !wb_search_completion_entry_has_focus (self)) {
    wb_search_completion_hide (self);
    return;
  }

  free_items (self);
  self->items = self->item_provider (text, MAX_ITEMS, self->user_data);
  self->n_items = self->items ? g_strv_length (self->items) : 0;
  listbox_remove_all (self);
  if (self->n_items == 0) {
    wb_search_completion_hide (self);
    return;
  }

  for (guint i = 0; i < self->n_items; i++)
    gtk_list_box_append (GTK_LIST_BOX (self->listbox), create_row (self->items[i]));

  if (!sync_geometry (self)) {
    wb_search_completion_hide (self);
    return;
  }

  set_entry_active (self, TRUE);
  gtk_popover_popup (GTK_POPOVER (self->popover));
  gtk_popover_present (GTK_POPOVER (self->popover));
  grab_focus_without_selecting (self->entry);
}

void
wb_search_completion_set_enabled (WbSearchCompletion *self, gboolean enabled)
{
  self->enabled = enabled;
  if (self->torn_down)
    return;
  if (enabled)
    wb_search_completion_update (self, gtk_editable_get_text (GTK_EDITABLE (self->entry)));
  else
    wb_search_completion_clear (self);
}

static gboolean
activate_index (WbSearchCompletion *self, int index)
{
  if (index < 0 || (guint) index >= self->n_items)
    return FALSE;
  char *item = g_strdup (self->items[index]);
  wb_search_completion_ref (self);
  wb_search_completion_clear (self);
  self->activate (item, self->user_data);
  wb_search_completion_unref (self);
  g_free (item);
  return TRUE;
}

static gboolean
move_selection (WbSearchCompletion *self, int step)
{
  if (self->n_items == 0)
    return FALSE;

  int index = selected_index (self, FALSE);
  if (index < 0)
    index = step > 0 ? 0 : (int) self->n_items - 1;
  else
    index = CLAMP (index + step, 0, (int) self->n_items - 1);

  GtkListBoxRow *row = gtk_list_box_get_row_at_index (GTK_LIST_BOX (self->listbox), index);
  if (row == NULL)
    return FALSE;
  gtk_list_box_select_row (GTK_LIST_BOX (self->listbox), row);
  return TRUE;
}

static void
apply_entry_text (WbSearchCompletion *self, const char *completion_text)
{
  const char *cur = gtk_editable_get_text (GTK_EDITABLE (self->entry));
  const char *p = cur;
  while (*p && g_unichar_isspace (g_utf8_get_char (p)))
    p = g_utf8_next_char (p);
  g_autofree char *leading = g_strndup (cur, p - cur);
  g_autofree char *updated = g_strconcat (leading, completion_text, NULL);
  gtk_editable_set_text (GTK_EDITABLE (self->entry), updated);
  gtk_editable_set_position (GTK_EDITABLE (self->entry), (int) g_utf8_strlen (updated, -1));
}

gboolean
wb_search_completion_handle_key_pressed (WbSearchCompletion *self, guint keyval)
{
  if (self->torn_down || !gtk_widget_get_visible (self->popover))
    return FALSE;

  switch (keyval) {
    case GDK_KEY_Down:
    case GDK_KEY_Up:
      return move_selection (self, keyval == GDK_KEY_Down ? 1 : -1);

    case GDK_KEY_Tab: {
      int index = selected_index (self, TRUE);
      if (index < 0)
        return FALSE;
      apply_entry_text (self, self->items[index]);
      wb_search_completion_hide (self);
      return TRUE;
    }
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
      return activate_index (self, selected_index (self, FALSE));

    case GDK_KEY_Escape:
      wb_search_completion_hide (self);
      return TRUE;

    default:
      return FALSE;
  }
}

static void
on_row_activated (GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
  activate_index (data, gtk_list_box_row_get_index (row));
}

static gboolean
on_key_pressed (GtkEventControllerKey *c, guint keyval, guint keycode,
                GdkModifierType state, gpointer data)
{
  return wb_search_completion_handle_key_pressed (data, keyval) ? GDK_EVENT_STOP
                                                                : GDK_EVENT_PROPAGATE;
}

static void
on_popover_closed (GtkPopover *p, gpointer data)
{
  set_entry_active (data, FALSE);
}

static void
on_entry_focus_enter (GtkEventControllerFocus *c, gpointer data)
{
  WbSearchCompletion *self = data;
  wb_search_completion_update (self, gtk_editable_get_text (GTK_EDITABLE (self->entry)));
}

static gboolean
idle_hide_if_focus_lost (gpointer data)
{
  WbSearchCompletion *self = data;
  if (!self->torn_down && !wb_search_completion_entry_has_focus (self))
    wb_search_completion_hide (self);
  wb_search_completion_unref (self);
  return G_SOURCE_REMOVE;
}

static void
on_entry_focus_leave (GtkEventControllerFocus *c, gpointer data)
{
  g_idle_add (idle_hide_if_focus_lost, wb_search_completion_ref (data));
}

static gboolean
idle_show_if_possible (gpointer data)
{
  WbSearchCompletion *self = data;
  if (!self->torn_down)
    wb_search_completion_update (self, gtk_editable_get_text (GTK_EDITABLE (self->entry)));
  wb_search_completion_unref (self);
  return G_SOURCE_REMOVE;
}

static void
on_parent_pressed (GtkGestureClick *g, int n_press, double x, double y, gpointer data)
{
  WbSearchCompletion *self = data;
  GtkWidget *clicked = gtk_widget_pick (self->parent, x, y, GTK_PICK_DEFAULT);
  if (clicked == NULL) {
    wb_search_completion_hide (self);
    return;
  }

  gboolean clicked_entry = contains_widget (GTK_WIDGET (self->entry), clicked);
  if (!gtk_widget_get_visible (self->popover)) {
    if (clicked_entry)
      g_idle_add (idle_show_if_possible, wb_search_completion_ref (self));
    return;
  }
  if (clicked_entry || contains_widget (self->popover, clicked))
    return;
  wb_search_completion_hide (self);
}

static void
teardown (WbSearchCompletion *self)
{
  if (self->torn_down)
    return;
  self->torn_down = TRUE;

  if (self->parent != NULL && self->parent_destroy_id)
    g_signal_handler_disconnect (self->parent, self->parent_destroy_id);
  self->parent_destroy_id = 0;

  if (self->entry != NULL) {
    gtk_widget_remove_css_class (GTK_WIDGET (self->entry), ENTRY_ACTIVE_CLASS);
    gtk_widget_remove_css_class (GTK_WIDGET (self->entry), ENTRY_CLASS);
    gtk_widget_remove_controller (GTK_WIDGET (self->entry), self->key_controller);
    gtk_widget_remove_controller (GTK_WIDGET (self->entry), self->focus_controller);
    g_object_remove_weak_pointer (G_OBJECT (self->entry), (gpointer *) &self->entry);
    self->entry = NULL;
  }
  if (self->parent != NULL) {
    gtk_widget_remove_controller (self->parent, self->click_controller);
    g_object_remove_weak_pointer (G_OBJECT (self->parent), (gpointer *) &self->parent);
    self->parent = NULL;
  }

  if (self->popover != NULL) {
    if (gtk_widget_get_parent (self->popover) != NULL)
      gtk_widget_unparent (self->popover);
    self->popover = self->scroller = self->listbox = NULL;
  }
  free_items (self);
}

static void
on_parent_destroy (GtkWidget *w, gpointer data)
{
  WbSearchCompletion *self = data;
  self->parent_destroy_id = 0;
  teardown (self);
}

WbSearchCompletion *
wb_search_completion_ref (WbSearchCompletion *self)
{
  g_atomic_int_inc (&self->ref_count);
  return self;
}

void
wb_search_completion_unref (WbSearchCompletion *self)
{
  if (!g_atomic_int_dec_and_test (&self->ref_count))
    return;
  teardown (self);
  g_free (self);
}

WbSearchCompletion *
wb_search_completion_new (GtkWidget *parent, GtkEntry *entry,
                          WbCompletionItemProvider item_provider,
                          WbCompletionActivate activate, gpointer user_data)
{
  g_return_val_if_fail (GTK_IS_WIDGET (parent) && GTK_IS_ENTRY (entry), NULL);
  g_return_val_if_fail (item_provider != NULL && activate != NULL, NULL);

  WbSearchCompletion *self = g_new0 (WbSearchCompletion, 1);
  self->ref_count = 1;
  self->parent = parent;
  self->entry = entry;
  g_object_add_weak_pointer (G_OBJECT (parent), (gpointer *) &self->parent);
  g_object_add_weak_pointer (G_OBJECT (entry), (gpointer *) &self->entry);
  self->item_provider = item_provider;
  self->activate = activate;
  self->user_data = user_data;

  gtk_widget_add_css_class (GTK_WIDGET (entry), ENTRY_CLASS);

  self->listbox = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (self->listbox), GTK_SELECTION_SINGLE);
  gtk_list_box_set_activate_on_single_click (GTK_LIST_BOX (self->listbox), TRUE);
  gtk_widget_set_focusable (self->listbox, FALSE);
  g_signal_connect (self->listbox, "row-activated", G_CALLBACK (on_row_activated), self);

  self->scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (self->scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (self->scroller), TRUE);
  gtk_widget_set_focusable (self->scroller, FALSE);
  gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (self->scroller), FALSE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (self->scroller), self->listbox);

  self->popover = gtk_popover_new ();
  gtk_popover_set_autohide (GTK_POPOVER (self->popover), FALSE);
  gtk_popover_set_has_arrow (GTK_POPOVER (self->popover), FALSE);
  gtk_popover_set_position (GTK_POPOVER (self->popover), GTK_POS_BOTTOM);
  gtk_widget_set_focusable (self->popover, FALSE);
  gtk_widget_add_css_class (self->popover, "completion-popover");
  g_signal_connect (self->popover, "closed", G_CALLBACK (on_popover_closed), self);
  gtk_popover_set_child (GTK_POPOVER (self->popover), self->scroller);
  gtk_widget_set_parent (self->popover, parent);

  self->key_controller = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (self->key_controller, GTK_PHASE_CAPTURE);
  g_signal_connect (self->key_controller, "key-pressed", G_CALLBACK (on_key_pressed), self);
  gtk_widget_add_controller (GTK_WIDGET (entry), self->key_controller);

  self->focus_controller = gtk_event_controller_focus_new ();
  g_signal_connect (self->focus_controller, "enter", G_CALLBACK (on_entry_focus_enter), self);
  g_signal_connect (self->focus_controller, "leave", G_CALLBACK (on_entry_focus_leave), self);
  gtk_widget_add_controller (GTK_WIDGET (entry), self->focus_controller);

  GtkGesture *click = gtk_gesture_click_new ();
  self->click_controller = GTK_EVENT_CONTROLLER (click);
  gtk_event_controller_set_propagation_phase (self->click_controller, GTK_PHASE_CAPTURE);
  g_signal_connect (click, "pressed", G_CALLBACK (on_parent_pressed), self);
  gtk_widget_add_controller (parent, self->click_controller);

  self->parent_destroy_id = g_signal_connect (parent, "destroy", G_CALLBACK (on_parent_destroy), self);
  return self;
}
