#pragma once

#include <glib.h>

G_BEGIN_DECLS

char    *wb_database_find_compressed (void);

char    *wb_database_extracted_path (void);

gboolean wb_database_needs_extraction (void);

void     wb_database_cleanup_old_versions (void);

gboolean wb_database_extract (const char *compressed_path);

gboolean wb_database_setup (void);

G_END_DECLS
