#define _XOPEN_SOURCE 700
#define _GNU_SOURCE
#include "database.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>
#include <zstd.h>

#include "constants.h"
#include "utils.h"

#define WN_DIR_PREFIX "wn-"

static char *
current_dir_name (void)
{
  return g_strdup_printf (WN_DIR_PREFIX "%s", WN_FILE_VERSION);
}

char *
wb_database_find_compressed (void)
{
  g_autofree char *filename = g_strdup_printf ("wn-%s.db.zst", WN_FILE_VERSION);

  const char *build_root = g_getenv ("MESON_BUILD_ROOT");
  if (build_root != NULL) {
    char *dev = g_build_filename (build_root, "data", filename, NULL);
    if (g_file_test (dev, G_FILE_TEST_IS_REGULAR)) {
      log_info ("Found compressed database (dev): %s", dev);
      return dev;
    }
    g_free (dev);
  }

  for (const char *const *d = g_get_system_data_dirs (); d && *d; d++) {
    char *p = g_build_filename (*d, "dictionary", filename, NULL);
    if (g_file_test (p, G_FILE_TEST_IS_REGULAR)) {
      log_info ("Found compressed database: %s", p);
      return p;
    }
    g_free (p);
  }

  log_warning ("No compressed database found for version %s", WN_FILE_VERSION);
  return NULL;
}

char *
wb_database_extracted_path (void)
{
  g_autofree char *dir = current_dir_name ();
  return g_build_filename (wb_data_dir (), dir, "wn.db", NULL);
}

gboolean
wb_database_needs_extraction (void)
{
  g_autofree char *p = wb_database_extracted_path ();
  gboolean exists = g_file_test (p, G_FILE_TEST_EXISTS);
  if (!exists)
    log_info ("Database extraction needed for version %s", WN_FILE_VERSION);
  return !exists;
}

static int
rm_cb (const char *path, const struct stat *sb, int flag, struct FTW *ftw)
{
  (void) sb; (void) flag; (void) ftw;
  return remove (path);
}

void
wb_database_cleanup_old_versions (void)
{
  const char *data = wb_data_dir ();
  g_autoptr (GDir) dir = g_dir_open (data, 0, NULL);
  if (dir == NULL)
    return;

  g_autofree char *current = current_dir_name ();
  const char *name;
  while ((name = g_dir_read_name (dir)) != NULL) {
    if (!g_str_has_prefix (name, WN_DIR_PREFIX) || strcmp (name, current) == 0)
      continue;
    g_autofree char *full = g_build_filename (data, name, NULL);
    if (!g_file_test (full, G_FILE_TEST_IS_DIR))
      continue;
    log_info ("Removing old database version: %s", full);
    if (nftw (full, rm_cb, 16, FTW_DEPTH | FTW_PHYS) != 0)
      log_error ("Failed to remove old database directory %s: %s", full,
                 g_strerror (errno));
  }
}

gboolean
wb_database_extract (const char *compressed_path)
{
  g_autofree char *db_path = wb_database_extracted_path ();
  g_autofree char *dir = g_path_get_dirname (db_path);
  g_autofree char *tmp_path = g_build_filename (dir, "wn.tmp", NULL);

  if (g_mkdir_with_parents (dir, 0755) != 0) {
    log_error ("Database extraction failed: cannot create %s: %s", dir, g_strerror (errno));
    return FALSE;
  }

  log_info ("Extracting database from %s to %s", compressed_path, tmp_path);

  FILE *src = fopen (compressed_path, "rb");
  if (src == NULL) {
    log_error ("Database extraction failed: %s: %s", compressed_path, g_strerror (errno));
    return FALSE;
  }
  int dst = open (tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (dst < 0) {
    log_error ("Database extraction failed: %s: %s", tmp_path, g_strerror (errno));
    fclose (src);
    return FALSE;
  }

  ZSTD_DStream *ds = ZSTD_createDStream ();
  size_t in_cap  = ZSTD_DStreamInSize ();
  size_t out_cap = ZSTD_DStreamOutSize ();
  char *in_buf  = g_malloc (in_cap);
  char *out_buf = g_malloc (out_cap);
  gboolean ok = (ds != NULL);
  size_t last_ret = 0;
  gboolean got_input = FALSE;

  if (ok) {
    size_t r = ZSTD_initDStream (ds);
    if (ZSTD_isError (r)) {
      log_error ("Database extraction failed: %s", ZSTD_getErrorName (r));
      ok = FALSE;
    }
  }

  {
    unsigned char head[32];
    size_t n = fread (head, 1, sizeof head, src);
    if (n > 0) {
      unsigned long long sz = ZSTD_getFrameContentSize (head, n);
      if (sz != ZSTD_CONTENTSIZE_UNKNOWN && sz != ZSTD_CONTENTSIZE_ERROR && sz > 0)
        (void) posix_fallocate (dst, 0, (off_t) sz);
    }
    rewind (src);
  }

  while (ok) {
    size_t n = fread (in_buf, 1, in_cap, src);
    if (n == 0) {
      if (ferror (src)) {
        log_error ("Database extraction failed: read error");
        ok = FALSE;
      }
      break;
    }
    got_input = TRUE;
    ZSTD_inBuffer in = { in_buf, n, 0 };
    while (in.pos < in.size) {
      ZSTD_outBuffer out = { out_buf, out_cap, 0 };
      last_ret = ZSTD_decompressStream (ds, &out, &in);
      if (ZSTD_isError (last_ret)) {
        log_error ("Database extraction failed: %s", ZSTD_getErrorName (last_ret));
        ok = FALSE;
        break;
      }
      for (size_t off = 0; off < out.pos;) {
        ssize_t w = write (dst, out_buf + off, out.pos - off);
        if (w < 0) {
          if (errno == EINTR) continue;
          log_error ("Database extraction failed: write: %s", g_strerror (errno));
          ok = FALSE;
          break;
        }
        off += (size_t) w;
      }
      if (!ok) break;
    }
  }
  if (ok && (!got_input || last_ret != 0)) {
    log_error ("Database extraction failed: truncated or empty archive");
    ok = FALSE;
  }

  if (ok && fsync (dst) != 0)
    log_warning ("fsync failed on %s: %s", tmp_path, g_strerror (errno));
  if (close (dst) != 0 && ok) {
    log_error ("Database extraction failed: close: %s", g_strerror (errno));
    ok = FALSE;
  }
  fclose (src);
  if (ds) ZSTD_freeDStream (ds);
  g_free (in_buf);
  g_free (out_buf);

  if (ok && g_rename (tmp_path, db_path) != 0) {
    log_error ("Database extraction failed: rename: %s", g_strerror (errno));
    ok = FALSE;
  }
  if (!ok) {
    g_unlink (tmp_path);
    return FALSE;
  }
  log_info ("Database extraction complete");
  return TRUE;
}

gboolean
wb_database_setup (void)
{
  if (!wb_database_needs_extraction ()) {
    log_info ("Database already up to date");
    return TRUE;
  }

  g_autofree char *compressed = wb_database_find_compressed ();
  if (compressed == NULL) {
    log_error ("No compressed database found - installation may be incomplete");
    return FALSE;
  }

  wb_database_cleanup_old_versions ();
  return wb_database_extract (compressed);
}
