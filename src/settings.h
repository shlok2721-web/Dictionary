#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  WB_ACCENT_US = 0,
  WB_ACCENT_GB = 1,
  WB_ACCENT_COUNT
} WbPronunciationAccent;

const char            *wb_accent_code         (WbPronunciationAccent a);
const char            *wb_accent_display_name (WbPronunciationAccent a);
WbPronunciationAccent  wb_accent_from_code    (const char *code);
WbPronunciationAccent  wb_accent_from_index   (int index);
static inline int      wb_accent_index        (WbPronunciationAccent a) { return (int) a; }

typedef struct _WbSettings WbSettings;

WbSettings *wb_settings_get (void);

gboolean wb_settings_get_live_search          (WbSettings *s);
void     wb_settings_set_live_search          (WbSettings *s, gboolean v);
gboolean wb_settings_get_double_click         (WbSettings *s);
void     wb_settings_set_double_click         (WbSettings *s, gboolean v);
gboolean wb_settings_get_auto_paste_on_launch (WbSettings *s);
void     wb_settings_set_auto_paste_on_launch (WbSettings *s, gboolean v);
WbPronunciationAccent wb_settings_get_pronunciations_accent (WbSettings *s);
void     wb_settings_set_pronunciations_accent (WbSettings *s, WbPronunciationAccent a);

gboolean wb_settings_get_gtk_dark_ui (WbSettings *s);
void     wb_settings_set_gtk_dark_ui (WbSettings *s, gboolean v);

char   **wb_settings_get_history   (WbSettings *s);
void     wb_settings_set_history   (WbSettings *s, const char *const *terms);
void     wb_settings_clear_history (WbSettings *s);

char   **wb_settings_get_favorites   (WbSettings *s);
void     wb_settings_set_favorites   (WbSettings *s, const char *const *terms);
void     wb_settings_add_favorite    (WbSettings *s, const char *term);
void     wb_settings_remove_favorite (WbSettings *s, const char *term);
gboolean wb_settings_is_favorite     (WbSettings *s, const char *term);

int      wb_settings_get_window_width  (WbSettings *s);
void     wb_settings_set_window_width  (WbSettings *s, int v);
int      wb_settings_get_window_height (WbSettings *s);
void     wb_settings_set_window_height (WbSettings *s, int v);

void wb_settings_save_state (WbSettings *s, const char *const *history,
                             int width, int height);

void wb_settings_freeze (WbSettings *s);
void wb_settings_thaw   (WbSettings *s);
void wb_settings_reset_to_defaults (WbSettings *s);

G_END_DECLS
