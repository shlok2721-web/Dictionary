#pragma once

#include <glib.h>

G_BEGIN_DECLS

#if GLIB_CHECK_VERSION (2, 82, 0)
#define wb_sort_array g_sort_array
#else
#define wb_sort_array g_qsort_with_data
#endif

const char *wb_config_dir  (void);
const char *wb_config_file (void);
const char *wb_data_dir    (void);

typedef enum {
  WB_LOG_DEBUG = 0,
  WB_LOG_INFO,
  WB_LOG_WARNING,
  WB_LOG_ERROR,
  WB_LOG_CRITICAL
} WbLogLevel;

void wb_log_init (gboolean debug);

void wb_log_impl (WbLogLevel level, const char *file, int line,
                  const char *fmt, ...) G_GNUC_PRINTF (4, 5);

gboolean wb_log_enabled (WbLogLevel level);

#define wb_log(level, ...)                                           \
  G_STMT_START {                                                     \
    if (wb_log_enabled (level))                                      \
      wb_log_impl (level, __FILE__, __LINE__, __VA_ARGS__);          \
  } G_STMT_END

#define log_debug(...)    wb_log (WB_LOG_DEBUG,    __VA_ARGS__)
#define log_info(...)     wb_log (WB_LOG_INFO,     __VA_ARGS__)
#define log_warning(...)  wb_log (WB_LOG_WARNING,  __VA_ARGS__)
#define log_error(...)    wb_log (WB_LOG_ERROR,    __VA_ARGS__)
#define log_critical(...) wb_log (WB_LOG_CRITICAL, __VA_ARGS__)

char *wb_str_strip (const char *s);

char *wb_str_strip_chars (const char *s, const char *chars);

gboolean wb_str_is_blank (const char *s);

G_END_DECLS
