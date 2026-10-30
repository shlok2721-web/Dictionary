#include "settings.h"

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>

#include "utils.h"

#define HISTORY_LIMIT 20

static const struct { const char *code, *name; } accents[WB_ACCENT_COUNT] = {
  { "us", "American English" },
  { "gb", "British English"  },
};

const char *
wb_accent_code (WbPronunciationAccent a)
{
  return accents[(a >= 0 && a < WB_ACCENT_COUNT) ? a : WB_ACCENT_US].code;
}

const char *
wb_accent_display_name (WbPronunciationAccent a)
{
  return accents[(a >= 0 && a < WB_ACCENT_COUNT) ? a : WB_ACCENT_US].name;
}

WbPronunciationAccent
wb_accent_from_code (const char *code)
{
  if (code != NULL)
    for (int i = 0; i < WB_ACCENT_COUNT; i++)
      if (strcmp (accents[i].code, code) == 0)
        return (WbPronunciationAccent) i;
  return WB_ACCENT_US;
}

WbPronunciationAccent
wb_accent_from_index (int index)
{
  return (index >= 0 && index < WB_ACCENT_COUNT) ? (WbPronunciationAccent) index
                                                 : WB_ACCENT_US;
}

struct _WbSettings {
  GMutex  lock;
  int     freeze_depth;
  gboolean dirty;

  gboolean live_search;
  gboolean double_click;
  gboolean auto_paste_on_launch;
  WbPronunciationAccent accent;
  gboolean force_dark_mode;
  GPtrArray *history;
  GPtrArray *favorites;
  int window_width;
  int window_height;
};

static void
set_defaults (WbSettings *s)
{
  s->live_search = TRUE;
  s->double_click = FALSE;
  s->auto_paste_on_launch = FALSE;
  s->accent = WB_ACCENT_US;
  s->force_dark_mode = FALSE;
  g_ptr_array_set_size (s->history, 0);
  g_ptr_array_set_size (s->favorites, 0);
  /* Wide enough to show 2-3 random word cards on the welcome page without
     feeling cramped, while still being a reasonable default on small
     displays. */
  s->window_width = 640;
  s->window_height = 760;
}

static void
add_string_array (JsonBuilder *b, const char *name, GPtrArray *arr)
{
  json_builder_set_member_name (b, name);
  json_builder_begin_array (b);
  for (guint i = 0; i < arr->len; i++)
    json_builder_add_string_value (b, g_ptr_array_index (arr, i));
  json_builder_end_array (b);
}

static void
save_locked (WbSettings *s)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();

  json_builder_begin_object (b);

  json_builder_set_member_name (b, "behavior");
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "live_search");
  json_builder_add_boolean_value (b, s->live_search);
  json_builder_set_member_name (b, "double_click");
  json_builder_add_boolean_value (b, s->double_click);
  json_builder_set_member_name (b, "pronunciations_accent");
  json_builder_add_string_value (b, wb_accent_code (s->accent));
  json_builder_set_member_name (b, "auto_paste_on_launch");
  json_builder_add_boolean_value (b, s->auto_paste_on_launch);
  json_builder_end_object (b);

  json_builder_set_member_name (b, "appearance");
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "force_dark_mode");
  json_builder_add_boolean_value (b, s->force_dark_mode);
  json_builder_end_object (b);

  json_builder_set_member_name (b, "state");
  json_builder_begin_object (b);
  add_string_array (b, "history", s->history);
  add_string_array (b, "favorites", s->favorites);
  json_builder_set_member_name (b, "window_width");
  json_builder_add_int_value (b, s->window_width);
  json_builder_set_member_name (b, "window_height");
  json_builder_add_int_value (b, s->window_height);
  json_builder_end_object (b);

  json_builder_end_object (b);

  g_autoptr (JsonNode) root = json_builder_get_root (b);
  g_autoptr (JsonGenerator) gen = json_generator_new ();
  json_generator_set_root (gen, root);
  json_generator_set_pretty (gen, TRUE);
  json_generator_set_indent (gen, 4);

  g_autoptr (GError) err = NULL;
  gsize len = 0;
  g_autofree char *data = json_generator_to_data (gen, &len);

  if (g_mkdir_with_parents (wb_config_dir (), 0700) != 0)
    log_error ("Failed to create config dir %s", wb_config_dir ());

  if (!g_file_set_contents (wb_config_file (), data, (gssize) len, &err)) {
    log_error ("Failed to save settings: %s", err->message);
    return;
  }
  s->dirty = FALSE;
  log_debug ("Settings saved successfully");
}

static void
changed_locked (WbSettings *s)
{
  s->dirty = TRUE;
  if (s->freeze_depth == 0)
    save_locked (s);
}

static gboolean
read_bool (JsonObject *o, const char *name, gboolean *out)
{
  if (!json_object_has_member (o, name))
    return TRUE;
  JsonNode *n = json_object_get_member (o, name);
  if (!JSON_NODE_HOLDS_VALUE (n))
    return FALSE;
  GType t = json_node_get_value_type (n);
  if (t == G_TYPE_BOOLEAN) {
    *out = json_node_get_boolean (n);
    return TRUE;
  }
  if (t == G_TYPE_INT64) {
    gint64 v = json_node_get_int (n);
    if (v == 0 || v == 1) {
      *out = (v == 1);
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean
read_int (JsonObject *o, const char *name, int *out)
{
  if (!json_object_has_member (o, name))
    return TRUE;
  JsonNode *n = json_object_get_member (o, name);
  if (!JSON_NODE_HOLDS_VALUE (n) || json_node_get_value_type (n) != G_TYPE_INT64)
    return FALSE;
  gint64 v = json_node_get_int (n);
  if (v < G_MININT || v > G_MAXINT)
    return FALSE;
  *out = (int) v;
  return TRUE;
}

static gboolean
read_string_array (JsonObject *o, const char *name, GPtrArray *out)
{
  if (!json_object_has_member (o, name))
    return TRUE;
  JsonNode *n = json_object_get_member (o, name);
  if (!JSON_NODE_HOLDS_ARRAY (n))
    return FALSE;
  JsonArray *a = json_node_get_array (n);
  guint len = json_array_get_length (a);
  for (guint i = 0; i < len; i++) {
    JsonNode *e = json_array_get_element (a, i);
    if (!JSON_NODE_HOLDS_VALUE (e) || json_node_get_value_type (e) != G_TYPE_STRING)
      return FALSE;
    g_ptr_array_add (out, g_strdup (json_node_get_string (e)));
  }
  return TRUE;
}

static JsonObject *
get_section (JsonObject *root, const char *name, gboolean *ok)
{
  if (!json_object_has_member (root, name))
    return NULL;
  JsonNode *n = json_object_get_member (root, name);
  if (!JSON_NODE_HOLDS_OBJECT (n)) {
    *ok = FALSE;
    return NULL;
  }
  return json_node_get_object (n);
}

static gboolean
load_from_json (WbSettings *s)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_autoptr (GError) err = NULL;

  if (!json_parser_load_from_file (parser, wb_config_file (), &err)) {
    log_error ("Failed to load JSON settings: %s", err->message);
    return FALSE;
  }
  JsonNode *rootn = json_parser_get_root (parser);
  if (rootn == NULL || !JSON_NODE_HOLDS_OBJECT (rootn)) {
    log_error ("Failed to load JSON settings: root is not an object");
    return FALSE;
  }
  JsonObject *root = json_node_get_object (rootn);

  gboolean ok = TRUE;
  JsonObject *beh = get_section (root, "behavior", &ok);
  JsonObject *app = get_section (root, "appearance", &ok);
  JsonObject *st  = get_section (root, "state", &ok);

  if (ok && beh) {
    ok = read_bool (beh, "live_search", &s->live_search) &&
         read_bool (beh, "double_click", &s->double_click) &&
         read_bool (beh, "auto_paste_on_launch", &s->auto_paste_on_launch);
    if (ok && json_object_has_member (beh, "pronunciations_accent")) {
      JsonNode *n = json_object_get_member (beh, "pronunciations_accent");
      if (JSON_NODE_HOLDS_VALUE (n) && json_node_get_value_type (n) == G_TYPE_STRING)
        s->accent = wb_accent_from_code (json_node_get_string (n));
      else
        ok = FALSE;
    }
  }
  if (ok && app)
    ok = read_bool (app, "force_dark_mode", &s->force_dark_mode);
  if (ok && st) {
    ok = read_string_array (st, "history", s->history) &&
         read_string_array (st, "favorites", s->favorites) &&
         read_int (st, "window_width", &s->window_width) &&
         read_int (st, "window_height", &s->window_height);
    if (ok && s->history->len > HISTORY_LIMIT)
      g_ptr_array_remove_range (s->history, 0, s->history->len - HISTORY_LIMIT);
  }

  if (!ok)
    log_error ("Failed to load JSON settings: validation error");
  else
    log_info ("Loaded settings from JSON configuration");
  return ok;
}

static WbSettings *
settings_new (void)
{
  WbSettings *s = g_new0 (WbSettings, 1);
  g_mutex_init (&s->lock);
  s->history   = g_ptr_array_new_with_free_func (g_free);
  s->favorites = g_ptr_array_new_with_free_func (g_free);
  set_defaults (s);

  s->freeze_depth = 1;
  if (g_mkdir_with_parents (wb_config_dir (), 0700) != 0)
    log_error ("Failed to create config dir %s", wb_config_dir ());

  if (g_file_test (wb_config_file (), G_FILE_TEST_EXISTS)) {
    if (!load_from_json (s)) {
      log_info ("Creating default settings");
      set_defaults (s);
      s->dirty = TRUE;
    }
  } else {
    s->dirty = TRUE;
  }
  s->freeze_depth = 0;
  if (s->dirty)
    save_locked (s);
  return s;
}

WbSettings *
wb_settings_get (void)
{
  static gsize once = 0;
  static WbSettings *instance;
  if (g_once_init_enter (&once)) {
    instance = settings_new ();
    g_once_init_leave (&once, 1);
  }
  return instance;
}

#define LOCKED(s) g_autoptr (GMutexLocker) _l G_GNUC_UNUSED = g_mutex_locker_new (&(s)->lock)

#define BOOL_ACCESSORS(fn, field)                                          \
  gboolean wb_settings_get_##fn (WbSettings *s)                            \
  { LOCKED (s); return s->field; }                                         \
  void wb_settings_set_##fn (WbSettings *s, gboolean v)                    \
  { LOCKED (s); s->field = !!v; changed_locked (s); }

BOOL_ACCESSORS (live_search, live_search)
BOOL_ACCESSORS (double_click, double_click)
BOOL_ACCESSORS (auto_paste_on_launch, auto_paste_on_launch)
BOOL_ACCESSORS (gtk_dark_ui, force_dark_mode)

WbPronunciationAccent
wb_settings_get_pronunciations_accent (WbSettings *s)
{
  LOCKED (s);
  return s->accent;
}

void
wb_settings_set_pronunciations_accent (WbSettings *s, WbPronunciationAccent a)
{
  LOCKED (s);
  s->accent = wb_accent_from_index ((int) a);
  changed_locked (s);
}

int
wb_settings_get_window_width (WbSettings *s)
{ LOCKED (s); return s->window_width; }

void
wb_settings_set_window_width (WbSettings *s, int v)
{ LOCKED (s); s->window_width = v; changed_locked (s); }

int
wb_settings_get_window_height (WbSettings *s)
{ LOCKED (s); return s->window_height; }

void
wb_settings_set_window_height (WbSettings *s, int v)
{ LOCKED (s); s->window_height = v; changed_locked (s); }

static char **
dup_list (GPtrArray *a)
{
  char **out = g_new (char *, a->len + 1);
  for (guint i = 0; i < a->len; i++)
    out[i] = g_strdup (g_ptr_array_index (a, i));
  out[a->len] = NULL;
  return out;
}

static void
replace_list (GPtrArray *a, const char *const *terms)
{
  g_ptr_array_set_size (a, 0);
  for (; terms != NULL && *terms != NULL; terms++)
    g_ptr_array_add (a, g_strdup (*terms));
}

static int
find_in_list (GPtrArray *a, const char *term)
{
  for (guint i = 0; i < a->len; i++)
    if (strcmp (g_ptr_array_index (a, i), term) == 0)
      return (int) i;
  return -1;
}

char **wb_settings_get_history (WbSettings *s)
{ LOCKED (s); return dup_list (s->history); }

void wb_settings_set_history (WbSettings *s, const char *const *terms)
{ LOCKED (s); replace_list (s->history, terms); changed_locked (s); }

void wb_settings_clear_history (WbSettings *s)
{ LOCKED (s); g_ptr_array_set_size (s->history, 0); changed_locked (s); }

char **wb_settings_get_favorites (WbSettings *s)
{ LOCKED (s); return dup_list (s->favorites); }

void wb_settings_set_favorites (WbSettings *s, const char *const *terms)
{ LOCKED (s); replace_list (s->favorites, terms); changed_locked (s); }

void
wb_settings_add_favorite (WbSettings *s, const char *term)
{
  LOCKED (s);
  if (find_in_list (s->favorites, term) < 0) {
    g_ptr_array_add (s->favorites, g_strdup (term));
    s->dirty = TRUE;
  }
}

void
wb_settings_remove_favorite (WbSettings *s, const char *term)
{
  LOCKED (s);
  int i = find_in_list (s->favorites, term);
  if (i >= 0) {
    g_ptr_array_remove_index (s->favorites, (guint) i);
    s->dirty = TRUE;
  }
}

gboolean
wb_settings_is_favorite (WbSettings *s, const char *term)
{
  LOCKED (s);
  return find_in_list (s->favorites, term) >= 0;
}

void
wb_settings_freeze (WbSettings *s)
{ LOCKED (s); s->freeze_depth++; }

void
wb_settings_thaw (WbSettings *s)
{
  LOCKED (s);
  if (s->freeze_depth > 0)
    s->freeze_depth--;
  if (s->freeze_depth == 0 && s->dirty)
    save_locked (s);
}

void
wb_settings_save_state (WbSettings *s, const char *const *history,
                        int width, int height)
{
  LOCKED (s);
  replace_list (s->history, history);
  s->window_width = width;
  s->window_height = height;
  s->dirty = TRUE;
  if (s->freeze_depth == 0)
    save_locked (s);
}

void
wb_settings_reset_to_defaults (WbSettings *s)
{
  LOCKED (s);
  log_info ("Resetting settings to defaults");
  set_defaults (s);
  s->dirty = TRUE;
  save_locked (s);
}
