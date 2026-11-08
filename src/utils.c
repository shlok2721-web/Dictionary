#include "utils.h"

#include <glib/gstdio.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static char *config_dir_s;
static char *config_file_s;
static char *data_dir_s;

static void
init_paths (void)
{
  static gsize once = 0;
  if (g_once_init_enter (&once)) {
    config_dir_s  = g_build_filename (g_get_user_config_dir (), "dictionary", NULL);
    config_file_s = g_build_filename (config_dir_s, "dictionary.json", NULL);
    data_dir_s    = g_build_filename (g_get_user_data_dir (), "dictionary", NULL);
    g_once_init_leave (&once, 1);
  }
}

const char *wb_config_dir  (void) { init_paths (); return config_dir_s;  }
const char *wb_config_file (void) { init_paths (); return config_file_s; }
const char *wb_data_dir    (void) { init_paths (); return data_dir_s;    }

static gint        log_threshold = WB_LOG_WARNING;
static GThread    *main_thread;

static const char *const level_names[] = {
  "DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"
};

void
wb_log_init (gboolean debug)
{
  if (main_thread == NULL)
    main_thread = g_thread_self ();
  g_atomic_int_set (&log_threshold, debug ? WB_LOG_DEBUG : WB_LOG_WARNING);
}

gboolean
wb_log_enabled (WbLogLevel level)
{
  return (gint) level >= g_atomic_int_get (&log_threshold);
}

void
wb_log_impl (WbLogLevel level, const char *file, int line, const char *fmt, ...)
{
  char ts[32];
  struct tm tm;
  time_t now = time (NULL);
  gint64 usec = g_get_real_time () % G_USEC_PER_SEC;

  localtime_r (&now, &tm);
  strftime (ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);

  char *base = g_path_get_basename (file);
  char *dot = strrchr (base, '.');
  if (dot) *dot = '\0';

  va_list ap;
  va_start (ap, fmt);
  char *msg = g_strdup_vprintf (fmt, ap);
  va_end (ap);

  GThread *self = g_thread_self ();
  char thread_name[32];
  if (main_thread == NULL || self == main_thread)
    g_strlcpy (thread_name, "MainThread", sizeof thread_name);
  else
    g_snprintf (thread_name, sizeof thread_name, "Thread-%p", (void *) self);

  fprintf (stderr, "%s,%03d - [%s] [%s] (%s:%d) %s\n", ts, (int) (usec / 1000),
           level_names[level], thread_name, base, line, msg);

  g_free (msg);
  g_free (base);
}

char *
wb_str_strip (const char *s)
{
  if (s == NULL)
    return g_strdup ("");

  const char *start = s;
  while (*start) {
    gunichar c = g_utf8_get_char (start);
    if (!g_unichar_isspace (c))
      break;
    start = g_utf8_next_char (start);
  }

  const char *end = start + strlen (start);
  while (end > start) {
    const char *prev = g_utf8_prev_char (end);
    if (!g_unichar_isspace (g_utf8_get_char (prev)))
      break;
    end = prev;
  }
  return g_strndup (start, end - start);
}

char *
wb_str_strip_chars (const char *s, const char *chars)
{
  if (s == NULL)
    return g_strdup ("");

  const char *start = s;
  while (*start) {
    gunichar c = g_utf8_get_char (start);
    if (g_utf8_strchr (chars, -1, c) == NULL)
      break;
    start = g_utf8_next_char (start);
  }

  const char *end = start + strlen (start);
  while (end > start) {
    const char *prev = g_utf8_prev_char (end);
    if (g_utf8_strchr (chars, -1, g_utf8_get_char (prev)) == NULL)
      break;
    end = prev;
  }
  return g_strndup (start, end - start);
}

gboolean
wb_str_is_blank (const char *s)
{
  if (s == NULL)
    return TRUE;
  for (; *s; s = g_utf8_next_char (s))
    if (!g_unichar_isspace (g_utf8_get_char (s)))
      return FALSE;
  return TRUE;
}
