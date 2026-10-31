#include "popup_window.h"

#include <glib/gi18n.h>
#include <gtk4-layer-shell/gtk4-layer-shell.h>

#include "base.h"
#include "constants.h"
#include "database.h"
#include "settings.h"
#include "utils.h"

#define MIN_POPUP_WIDTH  280
#define MIN_POPUP_HEIGHT 320
#define EDGE_MARGIN      12
#define AUTO_HIDE_SECONDS 5

typedef struct {
  GtkWidget *window;
  GtkWidget *title_label;
  GtkWidget *content_box;
  GtkWidget *scroller;
  GtkWidget *open_button;
  GtkWidget *spinner;

  WbWordnet *wn;
  gboolean   wn_loading;
  char      *pending_term;   /* term requested while wn was still loading */
  char      *current_term;

  /* Auto-hide: only armed for clipboard-triggered shows (see
     wb_popup_window_handle_clipboard_term), never for a manual tray-click
     or "View Full Entry" open. While armed, any pointer activity over the
     popup — "touching" it — resets the countdown back to the full
     AUTO_HIDE_SECONDS rather than letting it expire mid-read. */
  guint    autohide_source_id;
  gboolean autohide_mode;

  WbPopupOpenRequestCallback open_cb;
  gpointer                   open_cb_user_data;
} PopupState;

static PopupState popup;

static void render_term (const char *term);
static void start_or_reset_autohide_timer (void);
static void stop_autohide_timer (void);

/* ---------------------------------------------------------------------- */
/* WordNet instance: opened once, lazily, off the main thread              */
/* ---------------------------------------------------------------------- */

static void
wn_open_thread (GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
  if (!wb_database_setup ()) {
    g_task_return_pointer (task, NULL, NULL);
    return;
  }
  g_task_return_pointer (task, wb_get_wn_instance (), NULL);
}

static void
on_wn_ready (GObject *source, GAsyncResult *res, gpointer user_data)
{
  popup.wn = g_task_propagate_pointer (G_TASK (res), NULL);
  popup.wn_loading = FALSE;

  if (popup.wn == NULL) {
    if (popup.title_label)
      gtk_label_set_text (GTK_LABEL (popup.title_label), _("Dictionary database unavailable"));
    return;
  }

  if (popup.pending_term != NULL) {
    g_autofree char *term = g_steal_pointer (&popup.pending_term);
    render_term (term);
    /* The 5 s countdown started when the popup was shown, i.e. while the
       database was still loading; restart it now that there is actually
       something to read, or a slow first load eats the whole window. */
    if (popup.autohide_mode)
      start_or_reset_autohide_timer ();
  }
}

static void
ensure_wn_loading (void)
{
  if (popup.wn != NULL || popup.wn_loading)
    return;
  popup.wn_loading = TRUE;
  GTask *task = g_task_new (NULL, NULL, on_wn_ready, NULL);
  g_task_run_in_thread (task, wn_open_thread);
  g_object_unref (task);
}

/* ---------------------------------------------------------------------- */
/* Content rendering                                                       */
/* ---------------------------------------------------------------------- */

static void
clear_content (void)
{
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (popup.content_box)) != NULL)
    gtk_box_remove (GTK_BOX (popup.content_box), child);
}

static void
append_markup (const char *markup)
{
  GtkWidget *label = gtk_label_new (NULL);
  gtk_label_set_markup (GTK_LABEL (label), markup);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_label_set_selectable (GTK_LABEL (label), TRUE);
  gtk_widget_set_margin_bottom (label, 6);
  gtk_box_append (GTK_BOX (popup.content_box), label);
}

static void
render_term (const char *term)
{
  /* `term` may itself BE popup.current_term (wb_popup_window_toggle()
     re-showing the last lookup) — duplicate before freeing the old copy,
     not after, or the free below would pull the rug out from under the
     string we're still reading. */
  char *term_copy = g_strdup (term);
  g_free (popup.current_term);
  popup.current_term = term_copy;
  term = popup.current_term;

  gtk_label_set_text (GTK_LABEL (popup.title_label), term);

  if (popup.wn == NULL) {
    ensure_wn_loading ();
    g_free (popup.pending_term);
    popup.pending_term = g_strdup (term);
    clear_content ();
    append_markup (_("<i>Loading dictionary…</i>"));
    return;
  }

  clear_content ();

  const char *accent = wb_accent_code (wb_settings_get_pronunciations_accent (wb_settings_get ()));
  WbDefinition *def = wb_format_output (term, popup.wn, accent);

  if (def == NULL || !wb_definition_has_result (def)) {
    g_autofree char *esc = g_markup_escape_text (term, -1);
    g_autofree char *markup = g_strdup_printf (_("No definition found for <b>%s</b>."), esc);
    append_markup (markup);
    if (def != NULL)
      wb_definition_free (def);
    return;
  }

  for (int pos = 0; pos < WB_POS_COUNT; pos++) {
    guint n = 0;
    const WbSynsetData *synsets = wb_definition_get_synsets (def, (WbPos) pos, &n);
    if (synsets == NULL || n == 0)
      continue;

    for (guint i = 0; i < n; i++) {
      const WbSynsetData *s = &synsets[i];
      g_autofree char *pos_name = g_strdup (wb_pos_name ((WbPos) pos));
      g_autofree char *def_esc = g_markup_escape_text (s->definition ? s->definition : "", -1);
      g_autofree char *markup = g_strdup_printf ("<b>%s</b> \xe2\x80\x94 %s", pos_name, def_esc);
      append_markup (markup);

      if (s->examples != NULL && s->examples[0] != NULL) {
        g_autofree char *ex_esc = g_markup_escape_text (s->examples[0], -1);
        g_autofree char *ex_markup = g_strdup_printf ("<i>\xe2\x80\x9c%s\xe2\x80\x9d</i>", ex_esc);
        append_markup (ex_markup);
      }

      if (s->syn != NULL && s->syn[0] != NULL) {
        GString *joined = g_string_new (NULL);
        for (const char *const *p = s->syn; *p != NULL; p++) {
          if (joined->len > 0)
            g_string_append (joined, ", ");
          g_string_append (joined, *p);
        }
        g_autofree char *syn_esc = g_markup_escape_text (joined->str, -1);
        g_autofree char *syn_markup = g_strdup_printf (_("<small>Synonyms: %s</small>"), syn_esc);
        append_markup (syn_markup);
        g_string_free (joined, TRUE);
      }
    }
  }
}

/* ---------------------------------------------------------------------- */
/* Window construction & positioning                                       */
/* ---------------------------------------------------------------------- */

static void
on_close_clicked (GtkButton *btn, gpointer user_data)
{
  wb_popup_window_hide ();
}

static void
on_open_clicked (GtkButton *btn, gpointer user_data)
{
  if (popup.open_cb != NULL && popup.current_term != NULL)
    popup.open_cb (popup.current_term, popup.open_cb_user_data);
  wb_popup_window_hide ();
}

static gboolean
on_key_pressed (GtkEventControllerKey *ctrl, guint keyval, guint keycode,
                GdkModifierType state, gpointer user_data)
{
  if (keyval == GDK_KEY_Escape) {
    wb_popup_window_hide ();
    return TRUE;
  }
  return FALSE;
}

/* ---------------------------------------------------------------------- */
/* Auto-hide (clipboard-triggered shows only)                              */
/* ---------------------------------------------------------------------- */

static gboolean
on_autohide_elapsed (gpointer user_data)
{
  /* Clear our bookkeeping before calling hide(): hide() also calls
     stop_autohide_timer(), and by the time a GSource's own callback
     returns G_SOURCE_REMOVE, GLib has already detached it — trying to
     g_source_remove() it again from inside hide() would warn about a
     nonexistent source if we hadn't already zeroed the id here. */
  popup.autohide_source_id = 0;
  wb_popup_window_hide ();
  return G_SOURCE_REMOVE;
}

static void
start_or_reset_autohide_timer (void)
{
  if (popup.autohide_source_id != 0)
    g_source_remove (popup.autohide_source_id);
  popup.autohide_mode = TRUE;
  popup.autohide_source_id = g_timeout_add_seconds (AUTO_HIDE_SECONDS, on_autohide_elapsed, NULL);
}

static void
stop_autohide_timer (void)
{
  if (popup.autohide_source_id != 0) {
    g_source_remove (popup.autohide_source_id);
    popup.autohide_source_id = 0;
  }
  popup.autohide_mode = FALSE;
}

static void
on_popup_touched (GtkEventControllerMotion *ctrl, double x, double y, gpointer user_data)
{
  /* Any pointer activity over the popup counts as "touched": hovering in
     resets the countdown, and so does moving around while it's up. Clicks
     on the close/open buttons already dismiss the window directly, so
     there's nothing extra to wire up for those. */
  if (popup.autohide_mode)
    start_or_reset_autohide_timer ();
}

static void
build_window (void)
{
  popup.window = gtk_window_new ();
  gtk_window_set_decorated (GTK_WINDOW (popup.window), FALSE);
  gtk_window_set_resizable (GTK_WINDOW (popup.window), FALSE);
  gtk_widget_add_css_class (popup.window, "background");
  gtk_widget_add_css_class (popup.window, "dictionary-popup");

  if (gtk_layer_is_supported ()) {
    gtk_layer_init_for_window (GTK_WINDOW (popup.window));
    gtk_layer_set_namespace (GTK_WINDOW (popup.window), "dictionary-popup");
    gtk_layer_set_layer (GTK_WINDOW (popup.window), GTK_LAYER_SHELL_LAYER_TOP);
    gtk_layer_set_anchor (GTK_WINDOW (popup.window), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor (GTK_WINDOW (popup.window), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_set_margin (GTK_WINDOW (popup.window), GTK_LAYER_SHELL_EDGE_TOP, EDGE_MARGIN);
    gtk_layer_set_margin (GTK_WINDOW (popup.window), GTK_LAYER_SHELL_EDGE_RIGHT, EDGE_MARGIN);
    gtk_layer_set_keyboard_mode (GTK_WINDOW (popup.window), GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
  } else {
    /* Non-Wayland / non-layer-shell session: GTK4 has no portable way for
       a client to request its own on-screen position, so we fall back to
       whatever placement the window manager picks. The window is still
       fully functional, just not pinned to a corner. */
    log_info ("Popup: layer-shell unavailable on this session; falling back "
             "to default window placement");
  }

  GtkWidget *root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (root, "card");
  gtk_window_set_child (GTK_WINDOW (popup.window), root);

  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_start (header, 10);
  gtk_widget_set_margin_end (header, 6);
  gtk_widget_set_margin_top (header, 8);
  gtk_widget_set_margin_bottom (header, 4);

  popup.title_label = gtk_label_new (NULL);
  gtk_widget_add_css_class (popup.title_label, "title-4");
  gtk_label_set_xalign (GTK_LABEL (popup.title_label), 0.0);
  gtk_widget_set_hexpand (popup.title_label, TRUE);
  gtk_box_append (GTK_BOX (header), popup.title_label);

  GtkWidget *close_btn = gtk_button_new_from_icon_name ("window-close-symbolic");
  gtk_widget_add_css_class (close_btn, "flat");
  gtk_widget_add_css_class (close_btn, "circular");
  g_signal_connect (close_btn, "clicked", G_CALLBACK (on_close_clicked), NULL);
  gtk_box_append (GTK_BOX (header), close_btn);

  gtk_box_append (GTK_BOX (root), header);
  gtk_box_append (GTK_BOX (root), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

  popup.scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (popup.scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand (popup.scroller, TRUE);

  popup.content_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_margin_start (popup.content_box, 10);
  gtk_widget_set_margin_end (popup.content_box, 10);
  gtk_widget_set_margin_top (popup.content_box, 6);
  gtk_widget_set_margin_bottom (popup.content_box, 6);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (popup.scroller), popup.content_box);
  gtk_box_append (GTK_BOX (root), popup.scroller);

  gtk_box_append (GTK_BOX (root), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

  popup.open_button = gtk_button_new_with_label (_("View Full Entry"));
  gtk_widget_add_css_class (popup.open_button, "flat");
  gtk_widget_set_margin_start (popup.open_button, 6);
  gtk_widget_set_margin_end (popup.open_button, 6);
  gtk_widget_set_margin_top (popup.open_button, 4);
  gtk_widget_set_margin_bottom (popup.open_button, 4);
  g_signal_connect (popup.open_button, "clicked", G_CALLBACK (on_open_clicked), NULL);
  gtk_box_append (GTK_BOX (root), popup.open_button);

  GtkEventController *keys = gtk_event_controller_key_new ();
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key_pressed), NULL);
  gtk_widget_add_controller (popup.window, keys);

  GtkEventController *motion = gtk_event_controller_motion_new ();
  g_signal_connect (motion, "enter", G_CALLBACK (on_popup_touched), NULL);
  g_signal_connect (motion, "motion", G_CALLBACK (on_popup_touched), NULL);
  gtk_widget_add_controller (root, motion);
}

static void
apply_sizing (void)
{
  GdkDisplay *display = gdk_display_get_default ();
  GdkMonitor *monitor = NULL;
  if (display != NULL) {
    GListModel *monitors = gdk_display_get_monitors (display);
    if (monitors != NULL && g_list_model_get_n_items (monitors) > 0)
      monitor = g_list_model_get_item (monitors, 0);
  }

  int percent = wb_settings_get_popup_size_percent (wb_settings_get ());
  int width = MIN_POPUP_WIDTH;
  int height = MIN_POPUP_HEIGHT;

  if (monitor != NULL) {
    GdkRectangle geom;
    gdk_monitor_get_geometry (monitor, &geom);
    width = MAX (MIN_POPUP_WIDTH, geom.width * percent / 100);
    height = MAX (MIN_POPUP_HEIGHT, geom.height * percent * 7 / 400); /* ~1.75x the width ratio */
    width = MIN (width, geom.width - 2 * EDGE_MARGIN);
    height = MIN (height, geom.height - 2 * EDGE_MARGIN);
    g_object_unref (monitor);
  }

  gtk_window_set_default_size (GTK_WINDOW (popup.window), width, height);
}

/* ---------------------------------------------------------------------- */
/* Public API                                                              */
/* ---------------------------------------------------------------------- */

void
wb_popup_window_init (WbPopupOpenRequestCallback open_request_cb, gpointer user_data)
{
  popup.open_cb = open_request_cb;
  popup.open_cb_user_data = user_data;
  build_window ();
  ensure_wn_loading ();
}

void
wb_popup_window_show_term (const char *term)
{
  if (popup.window == NULL)
    build_window ();

  /* A plain show (tray click, "View Full Entry", toggle re-show) is
     persistent by default — it stays open until explicitly closed.
     Auto-hide is opt-in, armed only by the clipboard path below, so any
     direct call here cancels whatever auto-hide state was left over from
     an earlier clipboard-triggered popup. */
  stop_autohide_timer ();

  apply_sizing ();
  render_term (term);
  gtk_window_present (GTK_WINDOW (popup.window));
}

void
wb_popup_window_toggle (void)
{
  if (popup.window != NULL && gtk_widget_get_visible (popup.window)) {
    wb_popup_window_hide ();
    return;
  }

  /* No specific term requested (plain tray click): re-show the last
     lookup if there is one, otherwise show a friendly empty state. */
  if (popup.current_term != NULL)
    wb_popup_window_show_term (popup.current_term);
  else {
    if (popup.window == NULL)
      build_window ();
    stop_autohide_timer ();
    apply_sizing ();
    clear_content ();
    gtk_label_set_text (GTK_LABEL (popup.title_label), _("Dictionary"));
    append_markup (_("Copy a word anywhere to look it up here."));
    gtk_window_present (GTK_WINDOW (popup.window));
  }
}

void
wb_popup_window_hide (void)
{
  stop_autohide_timer ();
  if (popup.window != NULL)
    gtk_widget_set_visible (popup.window, FALSE);
}

gboolean
wb_popup_window_is_visible (void)
{
  return popup.window != NULL && gtk_widget_get_visible (popup.window);
}

void
wb_popup_window_handle_clipboard_term (const char *term, gpointer user_data)
{
  if (!wb_settings_get_auto_show_popup (wb_settings_get ()))
    return;

  /* show_term() always clears auto-hide state first (see its comment), so
     arming it has to happen after, not before. */
  wb_popup_window_show_term (term);
  start_or_reset_autohide_timer ();
}
