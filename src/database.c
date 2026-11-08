#define _XOPEN_SOURCE 700
#define _GNU_SOURCE
#include "database.h"

#include <errno.h>
#include <ftw.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

#include "constants.h"
#include "settings.h"
#include "utils.h"

static GMutex  db_lock;
static char   *db_path;        /* resolved path, protected by db_lock */
static gboolean legacy_swept;  /* legacy cleanup runs at most once per process */

static gboolean
is_regular (const char *p)
{
  return g_file_test (p, G_FILE_TEST_IS_REGULAR);
}

char *
wb_database_uri (const char *path)
{
  g_autofree char *file_uri = g_filename_to_uri (path, NULL, NULL);
  return file_uri ? g_strconcat (file_uri, "?immutable=1", NULL) : NULL;
}

/* Every table the app queries (see the SQL in base.c). */
static const char *const required_tables[] = {
  "entries", "form_index", "senses", "sense_examples",
  "pron", "links", "lemma_rel", "suggest",
  "forms", "collocations", "example_top", "sentences",
  "sources", "license_notice", "meta",
};

gboolean
wb_database_validate (const char *path, char **error)
{
  if (error) *error = NULL;

  if (path == NULL || !g_file_test (path, G_FILE_TEST_IS_REGULAR)) {
    if (error) *error = g_strdup (_("That file doesn't exist or isn't a regular file."));
    return FALSE;
  }

  /* Cheap check first: a compressed or unrelated file gets a clear message
   * instead of SQLite's generic "file is not a database". */
  static const char magic[] = "SQLite format 3";
  char head[sizeof magic - 1] = { 0 };
  FILE *f = fopen (path, "rb");
  if (f == NULL) {
    if (error) *error = g_strdup_printf (_("Can't read the file: %s"), g_strerror (errno));
    return FALSE;
  }
  size_t n = fread (head, 1, sizeof head, f);
  fclose (f);
  if (n != sizeof head || memcmp (head, magic, sizeof head) != 0) {
    if (error) *error = g_strdup (_("That isn't a SQLite database. If it ends in .zst or .xz, decompress it first."));
    return FALSE;
  }

  g_autofree char *uri = wb_database_uri (path);
  sqlite3 *db = NULL;
  if (sqlite3_open_v2 (uri ? uri : path, &db,
                       SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX |
                       (uri ? SQLITE_OPEN_URI : 0), NULL) != SQLITE_OK) {
    if (error) *error = g_strdup_printf (_("Can't open the database: %s"),
                                         db ? sqlite3_errmsg (db) : "out of memory");
    sqlite3_close (db);
    return FALSE;
  }

  gboolean ok = TRUE;
  for (gsize i = 0; ok && i < G_N_ELEMENTS (required_tables); i++) {
    sqlite3_stmt *st = NULL;
    gboolean present = FALSE;
    if (sqlite3_prepare_v2 (db,
          "SELECT 1 FROM sqlite_master WHERE type IN ('table','view') AND name = ?1",
          -1, &st, NULL) == SQLITE_OK) {
      sqlite3_bind_text (st, 1, required_tables[i], -1, SQLITE_STATIC);
      present = sqlite3_step (st) == SQLITE_ROW;
    }
    sqlite3_finalize (st);
    if (!present) {
      if (error) *error = g_strdup_printf (_("This doesn't look like a Krumeto dictionary database (missing table “%s”)."),
                                           required_tables[i]);
      ok = FALSE;
    }
  }
  sqlite3_close (db);
  return ok;
}

char *
wb_database_find (void)
{
  g_autofree char *filename = g_strdup_printf (WB_DB_FILE_FMT, WN_FILE_VERSION);

  /* 0. Location chosen in Preferences. */
  g_autofree char *custom = wb_settings_get_database_path (wb_settings_get ());
  if (custom != NULL) {
    if (is_regular (custom)) {
      log_info ("Using database from Preferences: %s", custom);
      return g_steal_pointer (&custom);
    }
    log_warning ("Database chosen in Preferences not found (%s); falling back to automatic lookup",
                 custom);
  }

  /* 1. Development builds: <build root>/data/krumeto-<version>.db */
  const char *build_root = g_getenv ("MESON_BUILD_ROOT");
  if (build_root != NULL) {
    char *dev = g_build_filename (build_root, "data", filename, NULL);
    if (is_regular (dev)) {
      log_info ("Found database (dev): %s", dev);
      return dev;
    }
    g_free (dev);
  }

  /* 2. Per-user install: ~/.local/share/dictionary/krumeto-<version>.db
   *    Checked explicitly so it works even when XDG_DATA_DIRS does not
   *    include the user data directory. */
  char *user = g_build_filename (wb_data_dir (), filename, NULL);
  if (is_regular (user)) {
    log_info ("Found database: %s", user);
    return user;
  }
  g_free (user);

  /* 3. System-wide install: <XDG_DATA_DIRS>/dictionary/krumeto-<version>.db */
  for (const char *const *d = g_get_system_data_dirs (); d && *d; d++) {
    char *p = g_build_filename (*d, "dictionary", filename, NULL);
    if (is_regular (p)) {
      log_info ("Found database: %s", p);
      return p;
    }
    g_free (p);
  }

  log_warning ("No database found for version %s", WN_FILE_VERSION);
  return NULL;
}

const char *
wb_database_path (void)
{
  g_mutex_lock (&db_lock);
  const char *p = db_path;   /* never freed or replaced once set */
  g_mutex_unlock (&db_lock);
  return p;
}

static int
rm_cb (const char *path, const struct stat *sb, int flag, struct FTW *ftw)
{
  (void) sb; (void) flag; (void) ftw;
  return remove (path);
}

void
wb_database_cleanup_legacy (const char *keep_path)
{
  const char *data = wb_data_dir ();
  g_autoptr (GDir) dir = g_dir_open (data, 0, NULL);
  if (dir == NULL)
    return;

  const char *name;
  while ((name = g_dir_read_name (dir)) != NULL) {
    /* Only directories: the shipped krumeto-<version>.db files are regular
     * files and are never touched here. */
    gboolean ours = g_str_has_prefix (name, WB_DB_DIR_PREFIX) ||
                    g_str_has_prefix (name, "wn-");
    if (!ours)
      continue;
    g_autofree char *full = g_build_filename (data, name, NULL);
    if (!g_file_test (full, G_FILE_TEST_IS_DIR))
      continue;

    /* The person may have pointed Preferences at a database inside one of
     * these old folders (e.g. their previously extracted krumeto.db).
     * Never delete the file that is actually in use. */
    if (keep_path != NULL) {
      g_autofree char *prefix = g_strconcat (full, G_DIR_SEPARATOR_S, NULL);
      if (g_str_has_prefix (keep_path, prefix)) {
        log_info ("Keeping %s (contains the database in use)", full);
        continue;
      }
    }
    log_info ("Removing extracted database copy from an older version: %s", full);
    if (nftw (full, rm_cb, 16, FTW_DEPTH | FTW_PHYS) != 0)
      log_error ("Failed to remove %s: %s", full, g_strerror (errno));
  }
}

gboolean
wb_database_setup (void)
{
  g_mutex_lock (&db_lock);

  if (db_path != NULL && is_regular (db_path)) {
    g_mutex_unlock (&db_lock);
    return TRUE;
  }

  char *found = wb_database_find ();
  if (found == NULL) {
    log_error ("No database found - installation may be incomplete");
    g_mutex_unlock (&db_lock);
    return FALSE;
  }

  /* Keep the previous string alive: wb_database_path() hands out the pointer
   * without a copy, so it is intentionally never freed (one tiny, bounded
   * allocation per distinct resolved path). */
  db_path = found;

  gboolean sweep = !legacy_swept;
  legacy_swept = TRUE;
  g_mutex_unlock (&db_lock);

  if (sweep)
    wb_database_cleanup_legacy (found);
  return TRUE;
}
