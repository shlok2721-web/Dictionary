#define _GNU_SOURCE
#include <getopt.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <libxml/xmlreader.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <zstd.h>

#define CHUNK (1024 * 1024)

static const char *const WORDNET_URLS[] = {
  "https://github.com/globalwordnet/english-wordnet/releases/download/2025-edition/english-wordnet-2025-plus.xml.gz",
  "https://en-word.net/static/english-wordnet-2025-plus.xml.gz",
};

static const char SCHEMA[] =
  "CREATE TABLE lexicons (rowid INTEGER PRIMARY KEY, specifier TEXT NOT NULL UNIQUE,"
  "  id TEXT NOT NULL, label TEXT NOT NULL, language TEXT NOT NULL, email TEXT NOT NULL,"
  "  license TEXT NOT NULL, version TEXT NOT NULL, url TEXT);"
  "CREATE TABLE relation_types (rowid INTEGER PRIMARY KEY, type TEXT NOT NULL UNIQUE);"
  "CREATE TABLE entries (rowid INTEGER PRIMARY KEY, id TEXT NOT NULL,"
  "  lexicon_rowid INTEGER NOT NULL, pos TEXT NOT NULL);"
  "CREATE TABLE entry_index (entry_rowid INTEGER NOT NULL UNIQUE, lemma TEXT NOT NULL);"
  "CREATE TABLE forms (rowid INTEGER PRIMARY KEY, lexicon_rowid INTEGER NOT NULL,"
  "  entry_rowid INTEGER NOT NULL, form TEXT NOT NULL, normalized_form TEXT,"
  "  rank INTEGER DEFAULT 1);"
  "CREATE TABLE pronunciations (form_rowid INTEGER NOT NULL, lexicon_rowid INTEGER NOT NULL,"
  "  value TEXT, variety TEXT);"
  "CREATE TABLE synsets (rowid INTEGER PRIMARY KEY, id TEXT NOT NULL,"
  "  lexicon_rowid INTEGER NOT NULL, pos TEXT);"
  "CREATE TABLE synset_relations (rowid INTEGER PRIMARY KEY, lexicon_rowid INTEGER NOT NULL,"
  "  source_rowid INTEGER NOT NULL, target_rowid INTEGER NOT NULL, type_rowid INTEGER NOT NULL);"
  "CREATE TABLE definitions (rowid INTEGER PRIMARY KEY, lexicon_rowid INTEGER NOT NULL,"
  "  synset_rowid INTEGER NOT NULL, definition TEXT);"
  "CREATE TABLE synset_examples (rowid INTEGER PRIMARY KEY, lexicon_rowid INTEGER NOT NULL,"
  "  synset_rowid INTEGER NOT NULL, example TEXT);"
  "CREATE TABLE senses (rowid INTEGER PRIMARY KEY, id TEXT NOT NULL, lexicon_rowid INTEGER NOT NULL,"
  "  entry_rowid INTEGER NOT NULL, entry_rank INTEGER DEFAULT 1,"
  "  synset_rowid INTEGER NOT NULL, synset_rank INTEGER DEFAULT 1);"
  "CREATE TABLE sense_relations (rowid INTEGER PRIMARY KEY, lexicon_rowid INTEGER NOT NULL,"
  "  source_rowid INTEGER NOT NULL, target_rowid INTEGER NOT NULL, type_rowid INTEGER NOT NULL);"
  "CREATE TEMP TABLE p_sense (sense_rowid INTEGER, synset_id TEXT);"
  "CREATE TEMP TABLE p_srel (source INTEGER, type TEXT, target_id TEXT);"
  "CREATE TEMP TABLE p_syrel (source INTEGER, type TEXT, target_id TEXT);"
  "CREATE TEMP TABLE p_member (synset_id TEXT, entry_id TEXT, rank INTEGER, entry_rowid INTEGER);"
  "CREATE INDEX entries_id_tmp ON entries (id);";

static const char INDEXES[] =
  "CREATE INDEX entry_index_lemma_index ON entry_index (lemma);"
  "CREATE INDEX form_entry_index ON forms (entry_rowid);"
  "CREATE INDEX form_index ON forms (form);"
  "CREATE INDEX form_norm_index ON forms (normalized_form);"
  "CREATE INDEX pronunciation_form_index ON pronunciations (form_rowid);"
  "CREATE INDEX synset_id_index ON synsets (id);"
  "CREATE INDEX synset_relation_source_index ON synset_relations (source_rowid);"
  "CREATE INDEX definition_synset_index ON definitions (synset_rowid);"
  "CREATE INDEX synset_example_synset_index ON synset_examples (synset_rowid);"
  "CREATE INDEX sense_id_index ON senses (id);"
  "CREATE INDEX sense_entry_rowid_index ON senses (entry_rowid);"
  "CREATE INDEX sense_synset_rowid_index ON senses (synset_rowid);"
  "CREATE INDEX sense_relation_source_index ON sense_relations (source_rowid);";

static gboolean
exec_sql (sqlite3 *db, const char *sql)
{
  char *err = NULL;
  if (sqlite3_exec (db, sql, NULL, NULL, &err) != SQLITE_OK) {
    fprintf (stderr, "✗ SQL error: %s\n", err);
    sqlite3_free (err);
    return FALSE;
  }
  return TRUE;
}

static sqlite3_stmt *
prep (sqlite3 *db, const char *sql)
{
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2 (db, sql, -1, &st, NULL) != SQLITE_OK) {
    fprintf (stderr, "✗ prepare failed: %s\n  %s\n", sqlite3_errmsg (db), sql);
    exit (1);
  }
  return st;
}

static char *
normalize_form (const char *s)
{
  g_autofree char *fold = g_utf8_casefold (s, -1);
  g_autofree char *nfkd = g_utf8_normalize (fold, -1, G_NORMALIZE_NFKD);
  GString *out = g_string_sized_new (strlen (nfkd));
  for (const char *p = nfkd; *p; p = g_utf8_next_char (p)) {
    gunichar c = g_utf8_get_char (p);
    if (g_unichar_combining_class (c) == 0)
      g_string_append_unichar (out, c);
  }
  return g_string_free (out, FALSE);
}

static void
bind_text_or_null (sqlite3_stmt *st, int i, const char *s)
{
  if (s) sqlite3_bind_text (st, i, s, -1, SQLITE_TRANSIENT);
  else   sqlite3_bind_null (st, i);
}

static char *
attr (xmlTextReaderPtr r, const char *name)
{
  xmlChar *v = xmlTextReaderGetAttribute (r, (const xmlChar *) name);
  char *out = v ? g_strdup ((char *) v) : NULL;
  xmlFree (v);
  return out;
}

static char *
read_text (xmlTextReaderPtr r)
{
  xmlChar *v = xmlTextReaderReadString (r);
  char *out = v ? g_strdup ((char *) v) : g_strdup ("");
  xmlFree (v);
  return out;
}

typedef struct {
  sqlite3 *db;
  sqlite3_stmt *lexicon, *entry, *entry_index, *form, *pron, *sense, *p_sense,
               *p_srel, *synset, *p_syrel, *definition, *example, *p_member, *entry_pos;
} Imp;

static gboolean
import_lmf (sqlite3 *db, const char *path)
{
  Imp m = { 0 };
  m.db = db;
  m.entry_pos   = prep (db, "UPDATE entries SET pos=? WHERE rowid=?");
  m.lexicon     = prep (db, "INSERT INTO lexicons (specifier,id,label,language,email,license,version,url) VALUES (?,?,?,?,?,?,?,?)");
  m.entry       = prep (db, "INSERT INTO entries (id,lexicon_rowid,pos) VALUES (?,?,?)");
  m.entry_index = prep (db, "INSERT OR IGNORE INTO entry_index VALUES (?,?)");
  m.form        = prep (db, "INSERT INTO forms (lexicon_rowid,entry_rowid,form,normalized_form,rank) VALUES (?,?,?,?,?)");
  m.pron        = prep (db, "INSERT INTO pronunciations VALUES (?,?,?,?)");
  m.sense       = prep (db, "INSERT INTO senses (id,lexicon_rowid,entry_rowid,entry_rank,synset_rowid,synset_rank) VALUES (?,?,?,?,0,1)");
  m.p_sense     = prep (db, "INSERT INTO p_sense VALUES (?,?)");
  m.p_srel      = prep (db, "INSERT INTO p_srel VALUES (?,?,?)");
  m.synset      = prep (db, "INSERT INTO synsets (id,lexicon_rowid,pos) VALUES (?,?,?)");
  m.p_syrel     = prep (db, "INSERT INTO p_syrel VALUES (?,?,?)");
  m.definition  = prep (db, "INSERT INTO definitions (lexicon_rowid,synset_rowid,definition) VALUES (?,?,?)");
  m.example     = prep (db, "INSERT INTO synset_examples (lexicon_rowid,synset_rowid,example) VALUES (?,?,?)");
  m.p_member    = prep (db, "INSERT INTO p_member VALUES (?,?,?,NULL)");

  xmlTextReaderPtr r = xmlReaderForFile (path, NULL, XML_PARSE_NONET | XML_PARSE_HUGE);
  if (r == NULL) {
    fprintf (stderr, "✗ Cannot open %s\n", path);
    return FALSE;
  }

  gint64 lexicon = 0, entry = 0, form_lemma = 0, synset = 0, sense = 0;
  int entry_rank = 0, form_rank = 0;
  gboolean in_lemma = FALSE;
  gint64 n_entries = 0, n_synsets = 0;
  int rc;

  while ((rc = xmlTextReaderRead (r)) == 1) {
    int type = xmlTextReaderNodeType (r);
    const char *name = (const char *) xmlTextReaderConstName (r);
    if (type == XML_READER_TYPE_END_ELEMENT) {
      if (!strcmp (name, "Lemma")) in_lemma = FALSE;
      else if (!strcmp (name, "LexicalEntry")) entry = 0;
      else if (!strcmp (name, "Sense")) sense = 0;
      continue;
    }
    if (type != XML_READER_TYPE_ELEMENT)
      continue;

    if (!strcmp (name, "Lexicon")) {
      g_autofree char *id = attr (r, "id"), *ver = attr (r, "version");
      g_autofree char *label = attr (r, "label"), *lang = attr (r, "language");
      g_autofree char *email = attr (r, "email"), *lic = attr (r, "license");
      g_autofree char *url = attr (r, "url");
      g_autofree char *spec = g_strdup_printf ("%s:%s", id ? id : "", ver ? ver : "");
      sqlite3_reset (m.lexicon);
      sqlite3_bind_text (m.lexicon, 1, spec, -1, SQLITE_TRANSIENT);
      bind_text_or_null (m.lexicon, 2, id ? id : "");
      bind_text_or_null (m.lexicon, 3, label ? label : "");
      bind_text_or_null (m.lexicon, 4, lang ? lang : "");
      bind_text_or_null (m.lexicon, 5, email ? email : "");
      bind_text_or_null (m.lexicon, 6, lic ? lic : "");
      bind_text_or_null (m.lexicon, 7, ver ? ver : "");
      bind_text_or_null (m.lexicon, 8, url);
      sqlite3_step (m.lexicon);
      lexicon = sqlite3_last_insert_rowid (db);
      printf ("  Lexicon: %s\n", spec);
    } else if (!strcmp (name, "LexicalEntry")) {
      g_autofree char *id = attr (r, "id"), *idx = attr (r, "index");
      sqlite3_reset (m.entry);
      sqlite3_bind_text (m.entry, 1, id, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64 (m.entry, 2, lexicon);
      sqlite3_bind_text (m.entry, 3, "", -1, SQLITE_STATIC);
      sqlite3_step (m.entry);
      entry = sqlite3_last_insert_rowid (db);
      entry_rank = 0;
      form_rank = 0;
      n_entries++;
      if (idx) {
        sqlite3_reset (m.entry_index);
        sqlite3_bind_int64 (m.entry_index, 1, entry);
        sqlite3_bind_text (m.entry_index, 2, idx, -1, SQLITE_TRANSIENT);
        sqlite3_step (m.entry_index);
      }
    } else if (!strcmp (name, "Lemma") || !strcmp (name, "Form")) {
      gboolean is_lemma = name[0] == 'L';
      g_autofree char *wf = attr (r, "writtenForm");
      if (is_lemma) {
        g_autofree char *pos = attr (r, "partOfSpeech");
        sqlite3_reset (m.entry_pos);
        sqlite3_bind_text (m.entry_pos, 1, pos ? pos : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64 (m.entry_pos, 2, entry);
        sqlite3_step (m.entry_pos);
      }
      g_autofree char *norm = normalize_form (wf ? wf : "");
      sqlite3_reset (m.form);
      sqlite3_bind_int64 (m.form, 1, lexicon);
      sqlite3_bind_int64 (m.form, 2, entry);
      sqlite3_bind_text (m.form, 3, wf ? wf : "", -1, SQLITE_TRANSIENT);
      bind_text_or_null (m.form, 4, (wf && strcmp (norm, wf) != 0) ? norm : NULL);
      sqlite3_bind_int (m.form, 5, is_lemma ? 0 : ++form_rank);
      sqlite3_step (m.form);
      form_lemma = sqlite3_last_insert_rowid (db);
      in_lemma = TRUE;
    } else if (!strcmp (name, "Pronunciation") && in_lemma) {
      g_autofree char *variety = attr (r, "variety");
      g_autofree char *value = read_text (r);
      sqlite3_reset (m.pron);
      sqlite3_bind_int64 (m.pron, 1, form_lemma);
      sqlite3_bind_int64 (m.pron, 2, lexicon);
      sqlite3_bind_text (m.pron, 3, value, -1, SQLITE_TRANSIENT);
      bind_text_or_null (m.pron, 4, variety);
      sqlite3_step (m.pron);
    } else if (!strcmp (name, "Sense")) {
      g_autofree char *id = attr (r, "id"), *syn = attr (r, "synset");
      sqlite3_reset (m.sense);
      sqlite3_bind_text (m.sense, 1, id, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64 (m.sense, 2, lexicon);
      sqlite3_bind_int64 (m.sense, 3, entry);
      sqlite3_bind_int (m.sense, 4, ++entry_rank);
      sqlite3_step (m.sense);
      sense = sqlite3_last_insert_rowid (db);
      sqlite3_reset (m.p_sense);
      sqlite3_bind_int64 (m.p_sense, 1, sense);
      bind_text_or_null (m.p_sense, 2, syn);
      sqlite3_step (m.p_sense);
    } else if (!strcmp (name, "SenseRelation") && sense) {
      g_autofree char *t = attr (r, "relType"), *tg = attr (r, "target");
      sqlite3_reset (m.p_srel);
      sqlite3_bind_int64 (m.p_srel, 1, sense);
      bind_text_or_null (m.p_srel, 2, t);
      bind_text_or_null (m.p_srel, 3, tg);
      sqlite3_step (m.p_srel);
    } else if (!strcmp (name, "Synset")) {
      g_autofree char *id = attr (r, "id"), *pos = attr (r, "partOfSpeech");
      g_autofree char *members = attr (r, "members");
      sqlite3_reset (m.synset);
      sqlite3_bind_text (m.synset, 1, id, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64 (m.synset, 2, lexicon);
      bind_text_or_null (m.synset, 3, pos);
      sqlite3_step (m.synset);
      synset = sqlite3_last_insert_rowid (db);
      n_synsets++;
      if (members) {
        char **ids = g_strsplit_set (members, " \t\n", -1);
        int rank = 0;
        for (char **p = ids; *p; p++) {
          if (**p == '\0') continue;
          sqlite3_reset (m.p_member);
          sqlite3_bind_text (m.p_member, 1, id, -1, SQLITE_TRANSIENT);
          sqlite3_bind_text (m.p_member, 2, *p, -1, SQLITE_TRANSIENT);
          sqlite3_bind_int (m.p_member, 3, ++rank);
          sqlite3_step (m.p_member);
        }
        g_strfreev (ids);
      }
    } else if (!strcmp (name, "Definition") && synset) {
      g_autofree char *t = read_text (r);
      sqlite3_reset (m.definition);
      sqlite3_bind_int64 (m.definition, 1, lexicon);
      sqlite3_bind_int64 (m.definition, 2, synset);
      sqlite3_bind_text (m.definition, 3, t, -1, SQLITE_TRANSIENT);
      sqlite3_step (m.definition);
    } else if (!strcmp (name, "Example") && synset) {
      g_autofree char *t = read_text (r);
      sqlite3_reset (m.example);
      sqlite3_bind_int64 (m.example, 1, lexicon);
      sqlite3_bind_int64 (m.example, 2, synset);
      sqlite3_bind_text (m.example, 3, t, -1, SQLITE_TRANSIENT);
      sqlite3_step (m.example);
    } else if (!strcmp (name, "SynsetRelation") && synset) {
      g_autofree char *t = attr (r, "relType"), *tg = attr (r, "target");
      sqlite3_reset (m.p_syrel);
      sqlite3_bind_int64 (m.p_syrel, 1, synset);
      bind_text_or_null (m.p_syrel, 2, t);
      bind_text_or_null (m.p_syrel, 3, tg);
      sqlite3_step (m.p_syrel);
    }
  }
  xmlFreeTextReader (r);

  sqlite3_stmt **all[] = { &m.lexicon, &m.entry, &m.entry_index, &m.form, &m.pron,
    &m.sense, &m.p_sense, &m.p_srel, &m.synset, &m.p_syrel, &m.definition,
    &m.example, &m.p_member, &m.entry_pos };
  for (gsize i = 0; i < G_N_ELEMENTS (all); i++)
    sqlite3_finalize (*all[i]);

  if (rc != 0) {
    fprintf (stderr, "✗ XML parse error\n");
    return FALSE;
  }
  printf ("  Parsed %" G_GINT64_FORMAT " entries, %" G_GINT64_FORMAT " synsets\n",
          n_entries, n_synsets);
  return lexicon != 0;
}

static gboolean
resolve_references (sqlite3 *db)
{
  return exec_sql (db,
    "CREATE INDEX p_sense_tmp ON p_sense (sense_rowid);"
    "CREATE INDEX synsets_id_tmp ON synsets (id);"
    "CREATE INDEX senses_id_tmp ON senses (id);"
    "INSERT OR IGNORE INTO relation_types (type)"
    "  SELECT type FROM p_srel UNION SELECT type FROM p_syrel;"
    "UPDATE senses SET synset_rowid ="
    "  (SELECT ss.rowid FROM p_sense ps JOIN synsets ss ON ss.id = ps.synset_id"
    "    WHERE ps.sense_rowid = senses.rowid);"
    "UPDATE p_member SET entry_rowid = (SELECT e.rowid FROM entries e WHERE e.id = p_member.entry_id);"
    "CREATE INDEX p_member_tmp ON p_member (synset_id, entry_rowid);"
    "UPDATE senses SET synset_rank = COALESCE((SELECT pm.rank FROM p_member pm"
    "   WHERE pm.synset_id = (SELECT id FROM synsets WHERE rowid = senses.synset_rowid)"
    "     AND pm.entry_rowid = senses.entry_rowid), 1);"
    "INSERT INTO synset_relations (lexicon_rowid, source_rowid, target_rowid, type_rowid)"
    "  SELECT ss.lexicon_rowid, p.source, t.rowid, rt.rowid FROM p_syrel p"
    "    JOIN synsets ss ON ss.rowid = p.source"
    "    JOIN synsets t ON t.id = p.target_id"
    "    JOIN relation_types rt ON rt.type = p.type ORDER BY p.rowid;"
    "INSERT INTO sense_relations (lexicon_rowid, source_rowid, target_rowid, type_rowid)"
    "  SELECT s.lexicon_rowid, p.source, t.rowid, rt.rowid FROM p_srel p"
    "    JOIN senses s ON s.rowid = p.source"
    "    JOIN senses t ON t.id = p.target_id"
    "    JOIN relation_types rt ON rt.type = p.type ORDER BY p.rowid;"
    "DROP TABLE p_sense; DROP TABLE p_srel; DROP TABLE p_syrel; DROP TABLE p_member;"
    "DROP INDEX entries_id_tmp; DROP INDEX synsets_id_tmp; DROP INDEX senses_id_tmp;");
}

static gboolean
download (const char *dest)
{
  for (gsize i = 0; i < G_N_ELEMENTS (WORDNET_URLS); i++) {
    printf ("Downloading from %s...\n", WORDNET_URLS[i]);
    char *argv[] = { "curl", "-fL", "--progress-bar", "-A", "Dictionary", "-o", (char *) dest,
                     (char *) WORDNET_URLS[i], NULL };
    int status = 0;
    GError *err = NULL;
    if (g_spawn_sync (NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_CHILD_INHERITS_STDIN,
                      NULL, NULL, NULL, NULL, &status, &err) &&
        g_spawn_check_wait_status (status, NULL)) {
      printf ("✓ Downloaded to %s\n", dest);
      return TRUE;
    }
    printf ("✗ Download failed: %s\n", err ? err->message : "curl error");
    g_clear_error (&err);
  }
  return FALSE;
}

static gboolean
compress_file (const char *src_path, const char *out_path, int level)
{
  FILE *src = fopen (src_path, "rb");
  g_autofree char *dir = g_path_get_dirname (out_path);
  g_mkdir_with_parents (dir, 0755);
  FILE *dst = fopen (out_path, "wb");
  if (!src || !dst) {
    fprintf (stderr, "✗ Compression failed: cannot open files\n");
    if (src) fclose (src);
    if (dst) fclose (dst);
    return FALSE;
  }
  fseek (src, 0, SEEK_END);
  long orig = ftell (src);
  rewind (src);
  printf ("Compressing %.1f MB (level %d)...\n", orig / (double) CHUNK, level);

  ZSTD_CCtx *cc = ZSTD_createCCtx ();
  ZSTD_CCtx_setParameter (cc, ZSTD_c_compressionLevel, level);
  ZSTD_CCtx_setParameter (cc, ZSTD_c_checksumFlag, 1);
  ZSTD_CCtx_setParameter (cc, ZSTD_c_nbWorkers, 0);
  ZSTD_CCtx_setPledgedSrcSize (cc, (unsigned long long) orig);

  size_t in_cap = CHUNK, out_cap = ZSTD_CStreamOutSize ();
  char *in = g_malloc (in_cap), *out = g_malloc (out_cap);
  gboolean ok = TRUE;
  for (;;) {
    size_t n = fread (in, 1, in_cap, src);
    gboolean last = n < in_cap;
    ZSTD_inBuffer ib = { in, n, 0 };
    size_t rem;
    do {
      ZSTD_outBuffer ob = { out, out_cap, 0 };
      rem = ZSTD_compressStream2 (cc, &ob, &ib, last ? ZSTD_e_end : ZSTD_e_continue);
      if (ZSTD_isError (rem)) { fprintf (stderr, "✗ Compression failed: %s\n", ZSTD_getErrorName (rem)); ok = FALSE; break; }
      if (fwrite (out, 1, ob.pos, dst) != ob.pos) { ok = FALSE; break; }
    } while (last ? rem != 0 : ib.pos < ib.size);
    if (!ok || last) break;
  }
  ZSTD_freeCCtx (cc);
  g_free (in); g_free (out);
  fclose (src);
  long comp = ftell (dst);
  if (fclose (dst) != 0) ok = FALSE;
  if (ok)
    printf ("✓ Compressed to %.1f MB (%.0f%% saved)\n", comp / (double) CHUNK,
            (1.0 - (double) comp / orig) * 100.0);
  return ok;
}

int
main (int argc, char **argv)
{
  int level = 15;
  const char *source = NULL, *output = NULL;
  gboolean keep_db = FALSE;

  static const struct option opts[] = {
    { "compression-level", required_argument, 0, 'l' },
    { "source-file",       required_argument, 0, 's' },
    { "output",            required_argument, 0, 'o' },
    { "keep-db",           no_argument,       0, 'k' },
    { "help",              no_argument,       0, 'h' },
    { 0 }
  };
  int c;
  while ((c = getopt_long (argc, argv, "l:s:o:kh", opts, NULL)) != -1) {
    switch (c) {
      case 'l': level = atoi (optarg); break;
      case 's': source = optarg; break;
      case 'o': output = optarg; break;
      case 'k': keep_db = TRUE; break;
      default:
        fprintf (stderr, "Usage: %s [--compression-level 1-22] [--source-file F] [--output F] [--keep-db]\n", argv[0]);
        return c == 'h' ? 0 : 2;
    }
  }
  if (level < 1 || level > 22) {
    fprintf (stderr, "--compression-level must be 1-22\n");
    return 2;
  }
  if (output == NULL)
    output = "data/wn.db.zst";

  printf ("──────────────────── Dictionary Database Generator ────────────────────\n");
  printf ("Source:      %s\n", source ? source : WORDNET_URLS[0]);
  printf ("Output:      %s\n", output);
  printf ("Compression: Level %d\n\n", level);

  g_autofree char *tmpdir = g_dir_make_tmp ("dictionary-wn-XXXXXX", NULL);
  if (tmpdir == NULL) { fprintf (stderr, "✗ Cannot create temp dir\n"); return 1; }
  g_autofree char *dl = g_build_filename (tmpdir, "english-wordnet.xml.gz", NULL);
  g_autofree char *dbp = g_build_filename (tmpdir, "wn.db", NULL);
  int ret = 1;

  if (source == NULL) {
    if (!download (dl)) { printf ("✗ All download URLs failed\n"); goto out; }
    source = dl;
  }

  printf ("Adding %s...\n", source);
  sqlite3 *db = NULL;
  if (sqlite3_open (dbp, &db) != SQLITE_OK) goto out;
  if (!exec_sql (db, "PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF; PRAGMA page_size=4096;"
                     "PRAGMA cache_size=-65536; BEGIN;") ||
      !exec_sql (db, SCHEMA) ||
      !import_lmf (db, source) ||
      !resolve_references (db) ||
      !exec_sql (db, INDEXES) ||
      !exec_sql (db, "COMMIT; ANALYZE;")) {
    printf ("✗ Failed to add source file\n");
    sqlite3_close (db);
    goto out;
  }
  exec_sql (db, "VACUUM;");
  sqlite3_close (db);
  printf ("✓ Added %s\n", source);

  if (!compress_file (dbp, output, level))
    goto out;
  if (keep_db) {
    g_autofree char *keep = g_strconcat (output, ".db", NULL);
    g_rename (dbp, keep);
    printf ("Kept uncompressed database at %s\n", keep);
  }
  printf ("\n✓ Database generation complete\n");
  ret = 0;

out:
  {
    g_autoptr (GDir) d = g_dir_open (tmpdir, 0, NULL);
    const char *n;
    while (d && (n = g_dir_read_name (d)) != NULL) {
      g_autofree char *p = g_build_filename (tmpdir, n, NULL);
      g_unlink (p);
    }
    g_rmdir (tmpdir);
  }
  return ret;
}
