#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Locate the Krumeto database without copying or decompressing it.
 *
 * Order:
 *   1. the location chosen in Preferences (if set and the file exists)
 *   2. $MESON_BUILD_ROOT/data/krumeto-<version>.db      (development)
 *   3. ~/.local/share/dictionary/krumeto-<version>.db   (per-user install)
 *   4. $XDG_DATA_DIRS/dictionary/krumeto-<version>.db   (system install)
 *
 * Returns a newly allocated path, or NULL. */
char       *wb_database_find (void);

/* Check that PATH is a readable Krumeto SQLite database with every table the
 * app needs.  On failure returns FALSE and, if ERROR is non-NULL, sets it to a
 * newly allocated, user-presentable message. */
gboolean    wb_database_validate (const char *path, char **error);

/* file:///...?immutable=1 URI for opening PATH read-only without locking or
 * creating sidecar files.  Newly allocated; NULL if PATH can't be converted. */
char       *wb_database_uri (const char *path);

/* Path of the database that wb_database_setup() resolved, or NULL if setup
 * has not succeeded yet.  Owned by the library; do not free. */
const char *wb_database_path (void);

/* Remove extracted copies left behind by older versions of the app
 * (<data dir>/krumeto-<version>/ and the legacy wn-<version>/), except
 * the one that contains KEEP_PATH (may be NULL). */
void        wb_database_cleanup_legacy (const char *keep_path);

/* Resolve the database in place.  Cheap and thread-safe; safe to call from
 * several threads and several times.  Returns FALSE if no database exists. */
gboolean    wb_database_setup (void);

G_END_DECLS
