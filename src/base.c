#define _GNU_SOURCE
#include "base.h"

#include <errno.h>
#include <malloc.h>
#include <glib/gstdio.h>
#include <poll.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <json-glib/json-glib.h>

#include "database.h"
#include "espeak_lib.h"
#include "utils.h"

void
wb_create_required_dirs (void)
{
  g_mkdir_with_parents (wb_config_dir (), 0755);
  g_mkdir_with_parents (wb_data_dir (), 0755);
}

char *
wb_clean_search_terms (const char *search_term)
{
  g_autofree char *stripped = wb_str_strip (search_term);
  g_autofree char *trimmed = wb_str_strip_chars (stripped, WB_SEARCH_TERM_CLEANUP_CHARS);

  GString *out = g_string_sized_new (strlen (trimmed));
  for (const char *p = trimmed; *p; p = g_utf8_next_char (p)) {
    gunichar c = g_utf8_get_char (p);
    if (g_utf8_strchr (WB_SEARCH_TERM_REPLACE_CHARS, -1, c) == NULL)
      g_string_append_unichar (out, c);
  }
  return g_string_free (out, FALSE);
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

static char *
normalize_lemma (const char *lemma)
{
  g_autofree char *tmp = g_strdup (lemma);
  for (char *p = tmp; *p; p++)
    if (*p == '_')
      *p = ' ';
  return wb_str_strip (tmp);
}

static const struct { const char *ipa; const char *esp; } ipa_map[] = {
  { "tʃ", "tS" }, { "dʒ", "dZ" }, { "eɪ", "eI" }, { "aɪ", "aI" }, { "ɔɪ", "OI" },
  { "aʊ", "aU" }, { "oʊ", "oU" }, { "əʊ", "@U" }, { "ɪə", "I@" }, { "eə", "e@" },
  { "ɛə", "e@" }, { "ʊə", "U@" }, { "iː", "i:" }, { "ɑː", "A:" }, { "ɔː", "O:" },
  { "uː", "u:" }, { "ɜː", "3:" },
  { "ˈ", "'" }, { "ˌ", "," }, { "ː", ":" }, { ".", "" }, { "‿", "" }, { "|", "" },
  { "‖", "" }, { "ɚ", "@r" }, { "ɝ", "3r" }, { "ɪ", "I" }, { "ɛ", "E" }, { "e", "e" },
  { "æ", "a" }, { "ɑ", "A" }, { "ɒ", "0" }, { "ɔ", "O" }, { "ʊ", "U" }, { "ʌ", "V" },
  { "ɜ", "3" }, { "ə", "@" }, { "ɐ", "@" }, { "a", "a" }, { "θ", "T" }, { "ð", "D" },
  { "ʃ", "S" }, { "ʒ", "Z" }, { "ŋ", "N" }, { "ɹ", "r" }, { "ɾ", "4" }, { "ɫ", "l" },
  { "ʔ", "?" }, { "ʍ", "W" }, { "x", "x" }, { "ç", "C" },
};

char *
wb_ipa_to_espeak (const char *ipa_string)
{
  g_autofree char *stripped = wb_str_strip (ipa_string);
  g_autofree char *s = wb_str_strip_chars (stripped, "/[]");
  GString *out = g_string_sized_new (strlen (s) + 4);
  g_string_append (out, "[[");

  const char *p = s;
  while (*p) {
    gboolean matched = FALSE;
    for (gsize i = 0; i < G_N_ELEMENTS (ipa_map); i++) {
      gsize kl = strlen (ipa_map[i].ipa);
      if (strncmp (p, ipa_map[i].ipa, kl) == 0) {
        g_string_append (out, ipa_map[i].esp);
        p += kl;
        matched = TRUE;
        break;
      }
    }
    if (matched)
      continue;
    if ((guchar) *p < 128)
      g_string_append_c (out, *p);
    p = g_utf8_next_char (p);
  }
  g_string_append (out, "]]");
  return g_string_free (out, FALSE);
}

struct _WbDefinition {
  char          *term;
  gboolean       found;
  GStringChunk  *chunk;
  GArray        *by_pos[WB_POS_COUNT];
  GPtrArray     *vectors;
  GPtrArray     *prons;
};

static WbDefinition *
definition_new (const char *term)
{
  WbDefinition *d = g_new0 (WbDefinition, 1);
  d->term = g_strdup (term);
  d->chunk = g_string_chunk_new (1024);
  d->vectors = g_ptr_array_new_with_free_func (g_free);
  d->prons = g_ptr_array_new_with_free_func (g_free);
  return d;
}

void
wb_definition_free (WbDefinition *d)
{
  if (d == NULL)
    return;
  for (int i = 0; i < WB_POS_COUNT; i++)
    if (d->by_pos[i])
      g_array_unref (d->by_pos[i]);
  g_ptr_array_unref (d->vectors);
  g_ptr_array_unref (d->prons);
  g_string_chunk_free (d->chunk);
  g_free (d->term);
  g_free (d);
}

const char *wb_definition_get_term (const WbDefinition *d) { return d->term; }
gboolean wb_definition_has_result (const WbDefinition *d) { return d->found; }

const WbSynsetData *
wb_definition_get_synsets (const WbDefinition *d, WbPos pos, guint *n_out)
{
  GArray *a = (pos >= 0 && pos < WB_POS_COUNT) ? d->by_pos[pos] : NULL;
  if (n_out)
    *n_out = a ? a->len : 0;
  return a ? (const WbSynsetData *) a->data : NULL;
}

static const char *const *
make_vector (WbDefinition *d, GPtrArray *items)
{
  const char **v = g_new (const char *, items->len + 1);
  for (guint i = 0; i < items->len; i++)
    v[i] = g_ptr_array_index (items, i);
  v[items->len] = NULL;
  g_ptr_array_add (d->vectors, v);
  return (const char *const *) v;
}

typedef struct {
  const WbPronunciation *pron;
  GPtrArray *synsets;
} TmpPronGroup;

typedef struct {
  const char *lemma;
  GArray *pgroups;
} TmpLemmaGroup;

static gboolean
pron_key_equal (const WbPronunciation *a, const WbPronunciation *b)
{
  const char *ia = a ? a->ipa : "", *ib = b ? b->ipa : "";
  gboolean fa = a ? a->is_fallback : FALSE, fb = b ? b->is_fallback : FALSE;
  return fa == fb && strcmp (ia, ib) == 0;
}

WbLemmaGroups *
wb_group_synsets_by_lemma (const WbSynsetData *synsets, guint n)
{
  GArray *lgs = g_array_new (FALSE, FALSE, sizeof (TmpLemmaGroup));
  GHashTable *index = g_hash_table_new (g_str_hash, g_str_equal);

  for (guint i = 0; i < n; i++) {
    const WbSynsetData *s = &synsets[i];
    gpointer idxp;
    TmpLemmaGroup *lg;
    if (g_hash_table_lookup_extended (index, s->name, NULL, &idxp)) {
      lg = &g_array_index (lgs, TmpLemmaGroup, GPOINTER_TO_UINT (idxp));
    } else {
      TmpLemmaGroup nl = { s->name, g_array_new (FALSE, FALSE, sizeof (TmpPronGroup)) };
      g_array_append_val (lgs, nl);
      g_hash_table_insert (index, (gpointer) s->name, GUINT_TO_POINTER (lgs->len - 1));
      lg = &g_array_index (lgs, TmpLemmaGroup, lgs->len - 1);
    }
    TmpPronGroup *pg = NULL;
    for (guint k = 0; k < lg->pgroups->len; k++) {
      TmpPronGroup *c = &g_array_index (lg->pgroups, TmpPronGroup, k);
      if (pron_key_equal (c->pron, s->pron)) { pg = c; break; }
    }
    if (pg == NULL) {
      TmpPronGroup np = { s->pron, g_ptr_array_new () };
      g_array_append_val (lg->pgroups, np);
      pg = &g_array_index (lg->pgroups, TmpPronGroup, lg->pgroups->len - 1);
    }
    g_ptr_array_add (pg->synsets, (gpointer) s);
  }

  WbLemmaGroups *out = g_new0 (WbLemmaGroups, 1);
  out->n_groups = lgs->len;
  out->groups = g_new0 (WbLemmaGroup, lgs->len);
  for (guint i = 0; i < lgs->len; i++) {
    TmpLemmaGroup *lg = &g_array_index (lgs, TmpLemmaGroup, i);
    WbLemmaGroup *og = &out->groups[i];
    og->lemma = lg->lemma;
    og->n_pronunciation_groups = lg->pgroups->len;
    og->pronunciation_groups = g_new0 (WbPronunciationGroup, lg->pgroups->len);
    for (guint k = 0; k < lg->pgroups->len; k++) {
      TmpPronGroup *pg = &g_array_index (lg->pgroups, TmpPronGroup, k);
      WbPronunciationGroup *op = &og->pronunciation_groups[k];
      op->pronunciation = pg->pron;
      op->n_synsets = pg->synsets->len;
      op->synsets = (const WbSynsetData **) g_ptr_array_free (pg->synsets, FALSE);
    }
    g_array_unref (lg->pgroups);
  }
  g_array_unref (lgs);
  g_hash_table_unref (index);
  return out;
}

void
wb_lemma_groups_free (WbLemmaGroups *g)
{
  if (g == NULL)
    return;
  for (guint i = 0; i < g->n_groups; i++) {
    for (guint k = 0; k < g->groups[i].n_pronunciation_groups; k++)
      g_free (g->groups[i].pronunciation_groups[k].synsets);
    g_free (g->groups[i].pronunciation_groups);
  }
  g_free (g->groups);
  g_free (g);
}

/* ------------------------------------------------------------------------ *
 * Krumeto native-schema lookup (the whole database is used as shipped)
 *
 *   entries   id, lemma, pos, cefr, cefr_pos, zipf, freq_bucket, hyphenation,
 *             etymology, usage_notes, wiki_url, transitive, countable, pos_rank
 *   senses    id, entry_id, ord, rank, gloss, parent_gloss, tags (JSON)
 *   sense_examples(sense_id, text)
 *   pron      entry_id, ipa, variety, audio_url, audio_credit, audio_license
 *   links     entry_id, kind, target, sense_id, ...
 *   lemma_rel src, src_pos, rel, dst   (derivation / pertainym / antonym / participle)
 *   form_index(form, entry_id)         inflected form -> entry
 *   suggest   term, kind               pre-built autocomplete list
 * ------------------------------------------------------------------------ */

#define WB_MAX_ENTRIES  64    /* entries (distinct lemma+pos) for one query */
#define WB_MAX_RELATED  500   /* sanity ceiling per relation list           */

typedef enum {
  REL_SYN, REL_ANT, REL_BROADER, REL_NARROWER, REL_COORD, REL_DERIVED, REL_RELATED,
  REL_PERTAIN, REL_PARTICIPLE,
  REL_COUNT
} RelKind;

static const struct { const char *kind; RelKind rel; } link_kinds[] = {
  { "syn", REL_SYN }, { "ant", REL_ANT }, { "hypernym", REL_BROADER },
  { "hyponym", REL_NARROWER }, { "coordinate", REL_COORD },
  { "derived", REL_DERIVED }, { "related", REL_RELATED },
};

static const struct { const char *rel; RelKind kind; } lemma_rel_kinds[] = {
  { "antonym", REL_ANT }, { "derivation", REL_DERIVED },
  { "pertainym", REL_PERTAIN }, { "participle", REL_PARTICIPLE },
};

struct _WbWordnet {
  sqlite3      *db;
  GMutex        lock;
  sqlite3_stmt *st_entries, *st_senses, *st_examples, *st_pron, *st_links, *st_lemma_rel;
  sqlite3_stmt *st_forms, *st_collocations, *st_extra_examples;
  sqlite3_stmt *st_random;           /* one entry by id, see wb_wordnet_random_lemmas */
  gint64        id_min, id_max;      /* entries.id bounds, 0/0 until first use */
  char         *path;
};

/* Page-cache budget per connection, in KiB.  The main window gets a roomy
 * cache because it is the thing the person is actively using; the popup /
 * background service gets a token one so an idle tray process stays tiny. */
#define WB_CACHE_KIB_FULL 16384
#define WB_CACHE_KIB_LITE 512

/* Hand freed heap pages back to the OS.  glibc keeps them otherwise, which is
 * why RSS never shrinks after a window is closed or a database is released. */
void
wb_release_memory (void)
{
  sqlite3_release_memory (G_MAXINT);
  malloc_trim (0);
}

static const char *
col_text (sqlite3_stmt *st, int i)
{
  const char *t = (const char *) sqlite3_column_text (st, i);
  return t ? t : "";
}

/* The typed word is either a headword (idx_entries_lemma is COLLATE NOCASE, so
 * this is an index lookup) or an inflected form.  Headword hits sort first,
 * then by pos_rank. ?1 = typed text, ?2 = lower-cased typed text. */
#define ENTRY_COLS \
  "e.id, e.pos, e.lemma, COALESCE(e.cefr_pos, e.cefr), e.zipf, e.freq_bucket," \
  " e.hyphenation, e.etymology, e.usage_notes, e.wiki_url, e.transitive, e.countable"

static const char SQL_ENTRIES[] =
  "SELECT " ENTRY_COLS ", 0 AS src, e.pos_rank AS pr FROM entries e"
  "  WHERE e.lemma = ?1 COLLATE NOCASE"
  " UNION "
  "SELECT " ENTRY_COLS ", 1 AS src, e.pos_rank AS pr FROM form_index fi"
  "  JOIN entries e ON e.id = fi.entry_id WHERE fi.form = ?2"
  " ORDER BY src, pr, id LIMIT " G_STRINGIFY (WB_MAX_ENTRIES);

static const char SQL_SENSES[] =
  "SELECT id, gloss, parent_gloss, tags FROM senses WHERE entry_id = ?1"
  " ORDER BY (rank IS NULL), rank, ord";

static const char SQL_EXAMPLES[] =
  "SELECT text FROM sense_examples WHERE sense_id = ?1 ORDER BY rowid";

static const char SQL_PRON[] =
  "SELECT ipa, variety, audio_url, audio_credit, audio_license FROM pron"
  " WHERE entry_id = ?1 ORDER BY rowid";

static const char SQL_LINKS[] =
  "SELECT kind, sense_id, target FROM links WHERE entry_id = ?1"
  " AND kind IN ('syn','ant','hypernym','hyponym','coordinate','derived','related')"
  " ORDER BY rowid";

static const char SQL_LEMMA_REL[] =
  "SELECT rel, dst FROM lemma_rel WHERE src = ?1 AND src_pos = ?2";

static const char SQL_FORMS[] =
  "SELECT form, tags FROM forms WHERE entry_id = ?1 ORDER BY rowid";

static const char SQL_COLLOCATIONS[] =
  "SELECT pattern, count FROM collocations WHERE lemma = ?1 ORDER BY count DESC LIMIT 8";

/* example_top is the dataset's own curated "best examples for this lemma"
 * list (lemma -> sentence_id, ranked by ord) - this joins straight to the
 * sentence text rather than re-deriving a quality heuristic ourselves. */
static const char SQL_EXTRA_EXAMPLES[] =
  "SELECT s.text FROM example_top et JOIN sentences s ON s.id = et.sentence_id"
  " WHERE et.lemma = ?1 ORDER BY et.ord LIMIT 5";

/* Random headword by primary-key position: seek to the first row at or after
 * a random id.  entries.id is the table's rowid, so this is a single b-tree
 * descent - no scan, no word list needed. */
static const char SQL_RANDOM_ENTRY[] =
  "SELECT lemma FROM entries WHERE id >= ?1 ORDER BY id LIMIT 1";

static sqlite3 *
open_db_readonly (const char *path, int cache_kib)
{
  sqlite3 *db = NULL;

  /* The file lives wherever it was installed - often a read-only location
   * such as /usr/share - and is never modified while the app runs.  Open it
   * through a URI with immutable=1 so SQLite does no locking and never tries
   * to create -shm/-wal/-journal files next to it. */
  g_autofree char *uri = wb_database_uri (path);

  if (sqlite3_open_v2 (uri ? uri : path, &db,
                       SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX |
                       (uri ? SQLITE_OPEN_URI : 0), NULL) != SQLITE_OK) {
    log_error ("Krumeto initialization failed: %s", db ? sqlite3_errmsg (db) : "out of memory");
    sqlite3_close (db);
    return NULL;
  }
  g_autofree char *pragmas = g_strdup_printf ("PRAGMA query_only=1; PRAGMA cache_size=-%d;"
                                              "PRAGMA mmap_size=0; PRAGMA temp_store=MEMORY;",
                                              cache_kib);
  sqlite3_exec (db, pragmas, NULL, NULL, NULL);
  return db;
}

void
wb_wordnet_free (WbWordnet *wn)
{
  if (wn == NULL)
    return;
  sqlite3_stmt **all[] = { &wn->st_entries, &wn->st_senses, &wn->st_examples,
                           &wn->st_pron, &wn->st_links, &wn->st_lemma_rel,
                           &wn->st_forms, &wn->st_collocations, &wn->st_extra_examples,
                           &wn->st_random };
  for (gsize i = 0; i < G_N_ELEMENTS (all); i++)
    sqlite3_finalize (*all[i]);
  sqlite3_close (wn->db);
  g_mutex_clear (&wn->lock);
  g_free (wn->path);
  g_free (wn);
  wb_release_memory ();
}

static WbWordnet *wn_open (gboolean lite);

WbWordnet *
wb_get_wn_instance (void)
{
  return wn_open (FALSE);
}

/* Same database, tiny page cache: for the popup / background service. */
WbWordnet *
wb_get_wn_instance_lite (void)
{
  return wn_open (TRUE);
}

static WbWordnet *
wn_open (gboolean lite)
{
  log_info ("Initializing Krumeto database...");
  const char *db_path = wb_database_path ();
  if (db_path == NULL) {
    log_error ("Krumeto initialization failed: database location unknown "
               "(wb_database_setup() has not succeeded)");
    return NULL;
  }
  WbWordnet *wn = g_new0 (WbWordnet, 1);
  g_mutex_init (&wn->lock);
  wn->path = g_strdup (db_path);

  wn->db = open_db_readonly (wn->path, lite ? WB_CACHE_KIB_LITE : WB_CACHE_KIB_FULL);
  if (wn->db == NULL)
    goto fail;

  struct { sqlite3_stmt **st; const char *sql; } prep[] = {
    { &wn->st_entries,  SQL_ENTRIES },   { &wn->st_senses,   SQL_SENSES },
    { &wn->st_examples, SQL_EXAMPLES },  { &wn->st_pron,     SQL_PRON },
    { &wn->st_links,    SQL_LINKS },     { &wn->st_lemma_rel, SQL_LEMMA_REL },
    { &wn->st_forms,    SQL_FORMS },     { &wn->st_collocations, SQL_COLLOCATIONS },
    { &wn->st_extra_examples, SQL_EXTRA_EXAMPLES },
    { &wn->st_random, SQL_RANDOM_ENTRY },
  };
  for (gsize i = 0; i < G_N_ELEMENTS (prep); i++)
    if (sqlite3_prepare_v3 (wn->db, prep[i].sql, -1, SQLITE_PREPARE_PERSISTENT,
                            prep[i].st, NULL) != SQLITE_OK) {
      log_error ("Krumeto initialization failed (statement %" G_GSIZE_FORMAT "): %s",
                 i, sqlite3_errmsg (wn->db));
      goto fail;
    }

  log_info ("Krumeto database (%s) ready.", WN_FILE_VERSION);
  return wn;

fail:
  wb_wordnet_free (wn);
  return NULL;
}

/* Settings store "gb"; Krumeto's pron.variety says "uk". */
static const char *
krumeto_variety (const char *accent)
{
  if (accent != NULL && g_ascii_strcasecmp (accent, "gb") == 0)
    return "uk";
  return (accent && accent[0]) ? accent : "us";
}

/* Best match tiers: 0 = exact variety, 1 = 'both', 2 = first row seen. */
typedef struct { char *ipa, *url, *credit, *license; } PronCand;

static const WbPronunciation *
pick_pronunciation (WbWordnet *wn, WbDefinition *d, gint64 entry_id, const char *accent)
{
  sqlite3_stmt *st = wn->st_pron;
  sqlite3_reset (st);
  sqlite3_bind_int64 (st, 1, entry_id);

  g_autofree char *want_s = wb_str_strip (krumeto_variety (accent));
  g_autofree char *want = g_utf8_casefold (want_s, -1);
  PronCand ipa[3] = { { 0 } }, aud[3] = { { 0 } };

  while (sqlite3_step (st) == SQLITE_ROW) {
    g_autofree char *i = wb_str_strip_chars (col_text (st, 0), "/[] ");
    const char *url = col_text (st, 2);
    g_autofree char *v1 = wb_str_strip (col_text (st, 1));
    g_autofree char *v2 = g_utf8_casefold (v1, -1);
    int tier = strcmp (v2, want) == 0 ? 0 : strcmp (v2, "both") == 0 ? 1 : 2;

    if (i[0] && ipa[tier].ipa == NULL)
      ipa[tier].ipa = g_string_chunk_insert_const (d->chunk, i);
    if (url[0] && aud[tier].url == NULL) {
      aud[tier].url = g_string_chunk_insert_const (d->chunk, url);
      aud[tier].credit = g_string_chunk_insert_const (d->chunk, col_text (st, 3));
      aud[tier].license = g_string_chunk_insert_const (d->chunk, col_text (st, 4));
    }
  }
  sqlite3_reset (st);

  const char *best_ipa = NULL;
  const PronCand *best_aud = NULL;
  for (int t = 0; t < 3; t++) {
    if (!best_ipa && ipa[t].ipa) best_ipa = ipa[t].ipa;
    if (!best_aud && aud[t].url) best_aud = &aud[t];
  }
  if (!best_ipa && !best_aud)
    return NULL;

  WbPronunciation *p = g_new0 (WbPronunciation, 1);
  p->ipa = best_ipa ? best_ipa : "";
  if (best_aud) {
    p->audio_url = best_aud->url;
    p->audio_credit = best_aud->credit[0] ? best_aud->credit : NULL;
    p->audio_license = best_aud->license[0] ? best_aud->license : NULL;
  }
  g_ptr_array_add (d->prons, p);
  return p;
}

static gboolean
vec_contains (GPtrArray *a, const char *s)
{
  for (guint i = 0; i < a->len; i++)
    if (strcmp (g_ptr_array_index (a, i), s) == 0)
      return TRUE;
  return FALSE;
}

typedef struct {
  gint64   id;
  char    *pos, *lemma, *cefr, *freq_bucket, *hyphenation, *etym, *usage, *wiki, *countable;
  double   zipf;
  gboolean transitive;
} EntryRow;

static void
entry_row_clear (EntryRow *e)
{
  g_free (e->pos); g_free (e->lemma); g_free (e->cefr); g_free (e->freq_bucket);
  g_free (e->hyphenation); g_free (e->etym); g_free (e->usage); g_free (e->wiki);
  g_free (e->countable);
}

typedef struct {
  gint64     id;
  char      *definition, *parent;
  GPtrArray *tags, *examples;
  GPtrArray *rel[REL_COUNT];
} SenseAcc;

static void
sense_acc_clear (SenseAcc *s)
{
  g_free (s->definition);
  g_free (s->parent);
  g_ptr_array_unref (s->tags);
  g_ptr_array_unref (s->examples);
  for (int i = 0; i < REL_COUNT; i++)
    g_ptr_array_unref (s->rel[i]);
}

static int
sense_index (GArray *senses, gint64 id)
{
  for (guint i = 0; i < senses->len; i++)
    if (g_array_index (senses, SenseAcc, i).id == id)
      return (int) i;
  return -1;
}

static void
add_related (WbDefinition *d, GPtrArray *list, const char *lemma, const char *entry_lower)
{
  if (list->len >= WB_MAX_RELATED)
    return;
  g_autofree char *nl = normalize_lemma (lemma);
  if (nl[0] == '\0')
    return;
  g_autofree char *nll = g_utf8_strdown (nl, -1);
  if (strcmp (nll, entry_lower) == 0)
    return;
  char *interned = g_string_chunk_insert_const (d->chunk, nl);
  if (!vec_contains (list, interned))
    g_ptr_array_add (list, interned);
}

/* senses.tags is a JSON array of strings, e.g. ["informal","US"]. */
static void
parse_tags (WbDefinition *d, GPtrArray *out, const char *json)
{
  if (json == NULL || json[0] != '[' || strcmp (json, "[]") == 0)
    return;
  g_autoptr (JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, json, -1, NULL))
    return;
  JsonNode *root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_ARRAY (root))
    return;
  JsonArray *arr = json_node_get_array (root);
  for (guint i = 0; i < json_array_get_length (arr); i++) {
    JsonNode *n = json_array_get_element (arr, i);
    if (JSON_NODE_HOLDS_VALUE (n) && json_node_get_value_type (n) == G_TYPE_STRING) {
      const char *t = json_node_get_string (n);
      if (t && t[0])
        g_ptr_array_add (out, g_string_chunk_insert_const (d->chunk, t));
    }
  }
}

static const char *
intern_nonempty (WbDefinition *d, const char *s)
{
  return (s && s[0]) ? g_string_chunk_insert_const (d->chunk, s) : NULL;
}

/* Builds one WbSynsetData per sense of one entry. Caller holds wn->lock. */
static gboolean
append_entry (WbWordnet *wn, WbDefinition *d, const EntryRow *er, const char *accent)
{
  WbPos pos;
  if (!wb_pos_from_krumeto_tag (er->pos, &pos))
    log_warning ("Unmapped Krumeto POS '%s' for lemma '%s'", er->pos, er->lemma);

  g_autofree char *lemma = normalize_lemma (er->lemma);
  g_autofree char *lemma_lower = g_utf8_strdown (lemma, -1);

  GArray *senses = g_array_new (FALSE, FALSE, sizeof (SenseAcc));
  g_array_set_clear_func (senses, (GDestroyNotify) sense_acc_clear);

  sqlite3_stmt *st = wn->st_senses;
  sqlite3_reset (st);
  sqlite3_bind_int64 (st, 1, er->id);
  while (sqlite3_step (st) == SQLITE_ROW) {
    SenseAcc s = { 0 };
    s.id = sqlite3_column_int64 (st, 0);
    const char *gloss = col_text (st, 1), *parent = col_text (st, 2);
    s.definition = g_strdup (gloss[0] ? gloss : "No definition available.");
    s.parent = parent[0] ? g_strdup (parent) : NULL;
    s.tags = g_ptr_array_new ();
    s.examples = g_ptr_array_new ();
    for (int i = 0; i < REL_COUNT; i++)
      s.rel[i] = g_ptr_array_new ();
    parse_tags (d, s.tags, col_text (st, 3));
    g_array_append_val (senses, s);
  }
  sqlite3_reset (st);

  if (senses->len == 0) {
    g_array_unref (senses);
    return FALSE;
  }

  for (guint i = 0; i < senses->len; i++) {
    SenseAcc *s = &g_array_index (senses, SenseAcc, i);
    st = wn->st_examples;
    sqlite3_reset (st);
    sqlite3_bind_int64 (st, 1, s->id);
    while (sqlite3_step (st) == SQLITE_ROW) {
      const char *t = col_text (st, 0);
      if (t[0])
        g_ptr_array_add (s->examples, g_string_chunk_insert (d->chunk, t));
    }
    sqlite3_reset (st);
  }

  /* Sense-scoped links go to their sense. Entry-level links (sense_id NULL) go
   * to the first sense, so they appear once rather than under every meaning. */
  st = wn->st_links;
  sqlite3_reset (st);
  sqlite3_bind_int64 (st, 1, er->id);
  while (sqlite3_step (st) == SQLITE_ROW) {
    const char *kind = col_text (st, 0);
    int rel = -1;
    for (gsize k = 0; k < G_N_ELEMENTS (link_kinds); k++)
      if (strcmp (kind, link_kinds[k].kind) == 0) { rel = link_kinds[k].rel; break; }
    if (rel < 0)
      continue;
    int si = 0;
    if (sqlite3_column_type (st, 1) != SQLITE_NULL) {
      si = sense_index (senses, sqlite3_column_int64 (st, 1));
      if (si < 0)
        si = 0;   /* sense id not under this entry: keep the relation anyway */
    }
    add_related (d, g_array_index (senses, SenseAcc, si).rel[rel], col_text (st, 2), lemma_lower);
  }
  sqlite3_reset (st);

  /* lemma_rel is lemma-level (no sense scope): show it on the first sense,
   * merged and de-duplicated with what links already gave us. */
  st = wn->st_lemma_rel;
  sqlite3_reset (st);
  sqlite3_bind_text (st, 1, er->lemma, -1, SQLITE_STATIC);
  sqlite3_bind_text (st, 2, er->pos, -1, SQLITE_STATIC);
  while (sqlite3_step (st) == SQLITE_ROW) {
    const char *rel = col_text (st, 0);
    for (gsize k = 0; k < G_N_ELEMENTS (lemma_rel_kinds); k++)
      if (strcmp (rel, lemma_rel_kinds[k].rel) == 0) {
        GPtrArray *target = g_array_index (senses, SenseAcc, 0).rel[lemma_rel_kinds[k].kind];
        add_related (d, target, col_text (st, 1), lemma_lower);
        break;
      }
  }
  sqlite3_reset (st);

  /* Word family: other inflected forms of this entry, skipping the
     headword itself (forms.tags is a JSON array, same shape as senses.tags). */
  GArray *forms_acc = g_array_new (FALSE, FALSE, sizeof (WbInflectedForm));
  st = wn->st_forms;
  sqlite3_reset (st);
  sqlite3_bind_int64 (st, 1, er->id);
  while (sqlite3_step (st) == SQLITE_ROW) {
    const char *form = col_text (st, 0);
    if (!form[0] || g_ascii_strcasecmp (form, lemma) == 0)
      continue;
    GPtrArray *tagv = g_ptr_array_new ();
    parse_tags (d, tagv, col_text (st, 1));
    WbInflectedForm wif = { .form = g_string_chunk_insert_const (d->chunk, form),
                            .tags = make_vector (d, tagv) };
    g_ptr_array_unref (tagv);
    g_array_append_val (forms_acc, wif);
  }
  sqlite3_reset (st);
  guint n_word_family = forms_acc->len;
  WbInflectedForm *word_family = NULL;
  if (n_word_family > 0) {
    word_family = g_new (WbInflectedForm, n_word_family);
    memcpy (word_family, forms_acc->data, n_word_family * sizeof (WbInflectedForm));
    g_ptr_array_add (d->vectors, word_family);
  }
  g_array_unref (forms_acc);

  /* Common collocations for this lemma (lemma-level, same scope as lemma_rel). */
  GArray *colloc_acc = g_array_new (FALSE, FALSE, sizeof (WbCollocation));
  st = wn->st_collocations;
  sqlite3_reset (st);
  sqlite3_bind_text (st, 1, er->lemma, -1, SQLITE_STATIC);
  while (sqlite3_step (st) == SQLITE_ROW) {
    WbCollocation c = { .pattern = g_string_chunk_insert_const (d->chunk, col_text (st, 0)),
                        .count   = sqlite3_column_int (st, 1) };
    g_array_append_val (colloc_acc, c);
  }
  sqlite3_reset (st);
  guint n_collocations = colloc_acc->len;
  WbCollocation *collocations = NULL;
  if (n_collocations > 0) {
    collocations = g_new (WbCollocation, n_collocations);
    memcpy (collocations, colloc_acc->data, n_collocations * sizeof (WbCollocation));
    g_ptr_array_add (d->vectors, collocations);
  }
  g_array_unref (colloc_acc);

  /* Curated extra example sentences from the sentence corpus. */
  GPtrArray *extra_ex_acc = g_ptr_array_new ();
  st = wn->st_extra_examples;
  sqlite3_reset (st);
  sqlite3_bind_text (st, 1, er->lemma, -1, SQLITE_STATIC);
  while (sqlite3_step (st) == SQLITE_ROW) {
    const char *t = col_text (st, 0);
    if (t[0])
      g_ptr_array_add (extra_ex_acc, g_string_chunk_insert (d->chunk, t));
  }
  sqlite3_reset (st);
  const char *const *extra_examples = make_vector (d, extra_ex_acc);
  g_ptr_array_unref (extra_ex_acc);

  const WbPronunciation *pron = pick_pronunciation (wn, d, er->id, accent);
  const char *name = g_string_chunk_insert_const (d->chunk, lemma);

  if (d->by_pos[pos] == NULL)
    d->by_pos[pos] = g_array_new (FALSE, FALSE, sizeof (WbSynsetData));

  for (guint i = 0; i < senses->len; i++) {
    SenseAcc *s = &g_array_index (senses, SenseAcc, i);
    WbSynsetData sd = {
      .name       = name,
      .definition = g_string_chunk_insert (d->chunk, s->definition),
      .examples   = make_vector (d, s->examples),
      .syn        = make_vector (d, s->rel[REL_SYN]),
      .ant        = make_vector (d, s->rel[REL_ANT]),
      .broader    = make_vector (d, s->rel[REL_BROADER]),
      .narrower   = make_vector (d, s->rel[REL_NARROWER]),
      .coordinate = make_vector (d, s->rel[REL_COORD]),
      .derived    = make_vector (d, s->rel[REL_DERIVED]),
      .related    = make_vector (d, s->rel[REL_RELATED]),
      .pertains   = make_vector (d, s->rel[REL_PERTAIN]),
      .participle = make_vector (d, s->rel[REL_PARTICIPLE]),
      .pron       = pron,
      .parent_definition = s->parent ? g_string_chunk_insert (d->chunk, s->parent) : NULL,
      .tags       = make_vector (d, s->tags),
      .cefr        = intern_nonempty (d, er->cefr),
      .zipf        = er->zipf,
      .freq_bucket = intern_nonempty (d, er->freq_bucket),
      .hyphenation = intern_nonempty (d, er->hyphenation),
      .etymology   = intern_nonempty (d, er->etym),
      .usage_notes = intern_nonempty (d, er->usage),
      .wiki_url    = intern_nonempty (d, er->wiki),
      .countable   = intern_nonempty (d, er->countable),
      .transitive  = er->transitive,
      .word_family    = word_family,
      .n_word_family  = n_word_family,
      .collocations   = collocations,
      .n_collocations = n_collocations,
      .extra_examples = extra_examples,
    };
    g_array_append_val (d->by_pos[pos], sd);
  }

  g_array_unref (senses);
  return TRUE;
}

static void
collect_entries (WbWordnet *wn, const char *spelling, GArray *rows, GHashTable *seen)
{
  g_autofree char *lower = g_utf8_strdown (spelling, -1);
  sqlite3_stmt *st = wn->st_entries;
  sqlite3_reset (st);
  sqlite3_bind_text (st, 1, spelling, -1, SQLITE_STATIC);
  sqlite3_bind_text (st, 2, lower, -1, SQLITE_STATIC);
  while (sqlite3_step (st) == SQLITE_ROW) {
    gint64 id = sqlite3_column_int64 (st, 0);
    if (g_hash_table_contains (seen, &id))
      continue;
    gint64 *key = g_memdup2 (&id, sizeof id);
    g_hash_table_add (seen, key);
    EntryRow e = {
      .id = id, .pos = g_strdup (col_text (st, 1)), .lemma = g_strdup (col_text (st, 2)),
      .cefr = g_strdup (col_text (st, 3)),
      .zipf = sqlite3_column_type (st, 4) == SQLITE_NULL ? -1.0 : sqlite3_column_double (st, 4),
      .freq_bucket = g_strdup (col_text (st, 5)), .hyphenation = g_strdup (col_text (st, 6)),
      .etym = g_strdup (col_text (st, 7)), .usage = g_strdup (col_text (st, 8)),
      .wiki = g_strdup (col_text (st, 9)), .transitive = sqlite3_column_int (st, 10) != 0,
      .countable = g_strdup (col_text (st, 11)),
    };
    g_array_append_val (rows, e);
  }
  sqlite3_reset (st);
}

WbDefinition *
wb_get_definition (const char *term, WbWordnet *wn, const char *accent)
{
  WbDefinition *d = definition_new (term);

  GArray *rows = g_array_new (FALSE, FALSE, sizeof (EntryRow));
  g_array_set_clear_func (rows, (GDestroyNotify) entry_row_clear);
  GHashTable *seen = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, NULL);

  g_mutex_lock (&wn->lock);

  g_autofree char *typed = wb_str_strip (term);
  collect_entries (wn, typed, rows, seen);

  if (rows->len == 0) {
    /* Last resort: accent/diacritic-folded spelling ("cafe" for "café"). */
    g_autofree char *norm = normalize_form (typed);
    if (strcmp (norm, typed) != 0)
      collect_entries (wn, norm, rows, seen);
  }

  char *first_match = NULL;
  for (guint i = 0; i < rows->len; i++) {
    const EntryRow *er = &g_array_index (rows, EntryRow, i);
    if (append_entry (wn, d, er, accent)) {
      d->found = TRUE;
      if (first_match == NULL) {
        g_autofree char *nl = normalize_lemma (er->lemma);
        first_match = g_string_chunk_insert_const (d->chunk, nl);
      }
    }
  }
  g_mutex_unlock (&wn->lock);

  if (d->found) {
    g_free (d->term);
    d->term = g_strdup (first_match ? first_match : term);
  }

  g_hash_table_destroy (seen);
  g_array_unref (rows);
  return d;
}

static int
run_capture (char **argv, int timeout_ms, char **out, gboolean *not_found)
{
  GPid pid;
  gint out_fd = -1, err_fd = -1;
  GError *err = NULL;

  if (not_found) *not_found = FALSE;
  if (out) *out = NULL;

  if (!g_spawn_async_with_pipes (NULL, argv, NULL,
                                 G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD |
                                 G_SPAWN_STDIN_FROM_DEV_NULL,
                                 NULL, NULL, &pid, NULL, &out_fd, &err_fd, &err)) {
    if (not_found && g_error_matches (err, G_SPAWN_ERROR, G_SPAWN_ERROR_NOENT))
      *not_found = TRUE;
    else if (err)
      log_error ("Failed to run %s: %s", argv[0], err->message);
    g_clear_error (&err);
    return -1;
  }

  GString *buf = g_string_new (NULL);
  struct pollfd fds[2] = { { out_fd, POLLIN, 0 }, { err_fd, POLLIN, 0 } };
  gint64 deadline = g_get_monotonic_time () + (gint64) timeout_ms * 1000;
  gboolean timed_out = FALSE;
  int open_fds = 2;

  while (open_fds > 0) {
    gint64 left = (deadline - g_get_monotonic_time ()) / 1000;
    if (left <= 0) { timed_out = TRUE; break; }
    int r = poll (fds, 2, (int) left);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) { timed_out = (r == 0); break; }
    for (int i = 0; i < 2; i++) {
      if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR)))
        continue;
      char tmp[4096];
      ssize_t n = read (fds[i].fd, tmp, sizeof tmp);
      if (n > 0) {
        if (i == 0) g_string_append_len (buf, tmp, n);
      } else if (n == 0 || errno != EINTR) {
        close (fds[i].fd);
        fds[i].fd = -1;
        open_fds--;
      }
    }
  }
  for (int i = 0; i < 2; i++)
    if (fds[i].fd >= 0) close (fds[i].fd);

  int status = 0, ret;
  if (timed_out) {
    kill (pid, SIGKILL);
    waitpid (pid, &status, 0);
    ret = -2;
  } else {
    waitpid (pid, &status, 0);
    ret = WIFEXITED (status) ? WEXITSTATUS (status) : -1;
  }
  g_spawn_close_pid (pid);

  if (out)
    *out = g_string_free (buf, FALSE);
  else
    g_string_free (buf, TRUE);
  return ret;
}

#define PRON_CACHE_SIZE 128

typedef struct { char *key; char *value; GList *link; } CacheEntry;

static GMutex     cache_lock;
static GHashTable *cache_map;
static GQueue     cache_order = G_QUEUE_INIT;

static void
cache_entry_free (CacheEntry *e)
{
  g_free (e->key);
  g_free (e->value);
  g_free (e);
}

static char *
pron_from_espeak (const char *term, const char *accent)
{
  const char *voice = (accent && strcmp (accent, "gb") == 0) ? "en-gb" : "en-us";
  char *argv[] = { "espeak-ng", "-v", (char *) voice, "--ipa=3", "-q", (char *) term, NULL };
  char *out = NULL;
  gboolean nf = FALSE;
  int rc = run_capture (argv, 10000, &out, &nf);

  if (nf) {
    /* No espeak-ng command on PATH: fall back to libespeak-ng, if present. */
    out = wb_espeak_lib_ipa (term, voice);
    if (out == NULL) {
      log_error ("'espeak-ng' command not found and no libespeak-ng library could be loaded. Please install espeak-ng.");
      return NULL;
    }
  } else if (rc == -2) {
    log_error ("espeak-ng timed out getting pronunciation for '%s'", term);
    g_free (out);
    return NULL;
  } else if (rc != 0 || out == NULL) {
    log_error ("espeak-ng failed with code %d", rc);
    g_free (out);
    return NULL;
  }

  g_autofree char *s = wb_str_strip (out);
  g_free (out);
  GString *g = g_string_new (NULL);
  for (const char *p = s; *p; p++)
    g_string_append_c (g, *p == '\n' ? ' ' : *p);
  GString *h = g_string_new (NULL);
  for (const char *p = g->str; *p;) {
    if (p[0] == ' ' && p[1] == ' ') { g_string_append_c (h, ' '); p += 2; }
    else { g_string_append_c (h, *p); p++; }
  }
  g_string_free (g, TRUE);
  char *res = wb_str_strip_chars (h->str, "/");
  g_string_free (h, TRUE);
  if (res[0] == '\0') { g_free (res); return NULL; }
  return res;
}

char *
wb_get_pronunciation (const char *term, const char *accent)
{
  g_autofree char *key = g_strdup_printf ("%s\x1f%s", accent ? accent : "", term);

  g_mutex_lock (&cache_lock);
  if (cache_map == NULL)
    cache_map = g_hash_table_new (g_str_hash, g_str_equal);
  CacheEntry *e = g_hash_table_lookup (cache_map, key);
  if (e) {
    g_queue_unlink (&cache_order, e->link);
    g_queue_push_head_link (&cache_order, e->link);
    char *r = g_strdup (e->value);
    g_mutex_unlock (&cache_lock);
    return r;
  }
  g_mutex_unlock (&cache_lock);

  char *res = pron_from_espeak (term, accent);

  g_mutex_lock (&cache_lock);
  if (!g_hash_table_contains (cache_map, key)) {
    e = g_new0 (CacheEntry, 1);
    e->key = g_strdup (key);
    e->value = g_strdup (res);
    g_queue_push_head (&cache_order, e);
    e->link = cache_order.head;
    g_hash_table_insert (cache_map, e->key, e);
    if (g_queue_get_length (&cache_order) > PRON_CACHE_SIZE) {
      CacheEntry *old = g_queue_pop_tail (&cache_order);
      g_hash_table_remove (cache_map, old->key);
      cache_entry_free (old);
    }
  }
  g_mutex_unlock (&cache_lock);
  return res;
}

static gboolean
needs_ipa_fallback (const WbSynsetData *s, const char *resolved_fold)
{
  if (s->pron != NULL && s->pron->ipa[0] != '\0')
    return FALSE;
  g_autofree char *nf = g_utf8_casefold (s->name, -1);
  return strcmp (nf, resolved_fold) == 0;
}

WbDefinition *
wb_fetch_definition (const char *term, WbWordnet *wn, const char *accent)
{
  WbDefinition *d = wb_get_definition (term, wn, accent);
  if (!d->found)
    return d;

  g_autofree char *resolved_fold = g_utf8_casefold (d->term, -1);
  gboolean needs = FALSE;
  for (int p = 0; p < WB_POS_COUNT && !needs; p++) {
    GArray *a = d->by_pos[p];
    for (guint i = 0; a && i < a->len && !needs; i++)
      needs = needs_ipa_fallback (&g_array_index (a, WbSynsetData, i), resolved_fold);
  }
  if (!needs)
    return d;

  char *ipa = wb_get_pronunciation (d->term, accent);
  if (ipa == NULL)
    return d;
  const char *fb_ipa = g_string_chunk_insert_const (d->chunk, ipa);
  g_free (ipa);

  /* One fallback pronunciation per original pron, so a recording (if the entry
   * has one) is kept alongside the generated IPA. */
  GHashTable *made = g_hash_table_new (NULL, NULL);
  for (int p = 0; p < WB_POS_COUNT; p++) {
    GArray *a = d->by_pos[p];
    for (guint i = 0; a && i < a->len; i++) {
      WbSynsetData *s = &g_array_index (a, WbSynsetData, i);
      if (!needs_ipa_fallback (s, resolved_fold))
        continue;
      gpointer key = (gpointer) s->pron;
      WbPronunciation *fb = g_hash_table_lookup (made, key);
      if (fb == NULL) {
        fb = g_new0 (WbPronunciation, 1);
        fb->ipa = fb_ipa;
        fb->is_fallback = TRUE;
        if (s->pron) {
          fb->audio_url = s->pron->audio_url;
          fb->audio_credit = s->pron->audio_credit;
          fb->audio_license = s->pron->audio_license;
        }
        g_ptr_array_add (d->prons, fb);
        g_hash_table_insert (made, key, fb);
      }
      s->pron = fb;
    }
  }
  g_hash_table_destroy (made);
  return d;
}

WbDefinition *
wb_format_output (const char *text, WbWordnet *wn, const char *accent)
{
  g_autofree char *cleaned = wb_clean_search_terms (text);
  if (cleaned[0] == '\0')
    return NULL;
  return wb_fetch_definition (cleaned, wn, accent);
}

/* System text-to-speech through speech-dispatcher (spd-say). Whatever voice
 * the user configured there (a neural Piper/RHVoice/Kokoro module, say) is used
 * as is, nothing is stored by the app. Returns TRUE if it spoke. Set
 * WB_TTS=espeak to skip this and use espeak-ng directly. */
static gboolean
speak_via_speechd (const char *text, int speed, int pitch)
{
  const char *force = g_getenv ("WB_TTS");
  if (force != NULL && g_strcmp0 (force, "espeak") == 0)
    return FALSE;

  /* spd-say scales are -100..100 with 0 = the module default. */
  g_autofree char *rate = g_strdup_printf ("%d", CLAMP ((speed - 175) * 100 / 150, -100, 100));
  g_autofree char *pit  = g_strdup_printf ("%d", CLAMP ((pitch - 50) * 2, -100, 100));
  char *argv[] = { "spd-say", "-w", "-r", rate, "-p", pit, "--", (char *) text, NULL };
  gboolean nf = FALSE;
  int rc = run_capture (argv, 15000, NULL, &nf);
  return !nf && rc == 0;
}

/* 1 = speak the dictionary's IPA through a crude IPA->espeak phoneme map
 * (flat, choppy prosody); 0 = let espeak-ng speak the word itself using its own
 * pronunciation rules, which sounds much smoother. The accent still selects
 * en-gb / en-us either way. */
#ifndef WB_SPEAK_FROM_IPA
#define WB_SPEAK_FROM_IPA 0
#endif

void
wb_read_term (const char *text, int speed, const char *accent, const char *ipa,
             const char *voice_variant, int pitch)
{
  g_autofree char *spoken = NULL;
#if WB_SPEAK_FROM_IPA
  if (ipa != NULL && ipa[0] != '\0')
    spoken = wb_ipa_to_espeak (ipa);
#else
  (void) ipa;
#endif
  const char *what = spoken ? spoken : text;
  const char *base_voice = (accent && strcmp (accent, "gb") == 0) ? "en-gb" : "en-us";
  /* Variant suffixes (e.g. "+f3") only reliably select a voice when they're
     appended directly to the base voice id in one "-v" argument - passing
     the variant alone, or as a separate SetVoiceByProperties call, misses
     more often than not. */
  g_autofree char *voice = g_strconcat (base_voice, voice_variant ? voice_variant : "", NULL);

  /* Preferred: the system TTS (speech-dispatcher), so the voice is whatever
     the user picked in their desktop settings. */
  if (speak_via_speechd (what, speed, pitch))
    return;

  /* Next: render with libespeak-ng, pad/fade the samples and play
     the finished file. Playing through espeak-ng's own live audio output
     underruns easily on PulseAudio/PipeWire, which is what makes speech sound
     stuttering and harsh. */
  if (wb_espeak_lib_speak (what, voice, speed, pitch))
    return;

  /* Fallback: the espeak-ng command, still rendered to a file first. */
  g_autofree char *tmp = NULL;
  g_autoptr (GError) err = NULL;
  int fd = g_file_open_tmp ("dictionary-speech-XXXXXX.wav", &tmp, &err);
  if (fd < 0) {
    log_error ("Could not create speech file: %s", err->message);
    return;
  }
  close (fd);

  g_autofree char *sp = g_strdup_printf ("%d", speed);
  g_autofree char *pp = g_strdup_printf ("%d", CLAMP (pitch, 0, 99));
  char *argv[] = { "espeak-ng", "-v", voice, "-s", sp, "-p", pp, "-w", tmp,
                   (char *) what, NULL };
  gboolean nf = FALSE;
  int rc = run_capture (argv, 10000, NULL, &nf);
  if (nf)
    log_error ("'espeak-ng' command not found and the libespeak-ng fallback could not speak. Please install espeak-ng.");
  else if (rc == -2)
    log_error ("espeak-ng timed out");
  else if (rc != 0)
    log_error ("espeak-ng failed with code %d", rc);
  else if (!wb_espeak_lib_play_file (tmp))
    log_error ("No audio player worked (tried pw-play, paplay, aplay, ffplay)");
  g_unlink (tmp);
}

typedef struct { char *text, *accent, *ipa, *voice_variant; int speed, pitch; } ReadJob;

static gpointer
read_thread (gpointer data)
{
  ReadJob *j = data;
  wb_read_term (j->text, j->speed, j->accent, j->ipa, j->voice_variant, j->pitch);
  g_free (j->text); g_free (j->accent); g_free (j->ipa); g_free (j->voice_variant); g_free (j);
  return NULL;
}

void
wb_read_term_async (const char *text, int speed, const char *accent, const char *ipa,
                    const char *voice_variant, int pitch)
{
  ReadJob *j = g_new0 (ReadJob, 1);
  j->text = g_strdup (text); j->accent = g_strdup (accent);
  j->ipa = g_strdup (ipa); j->speed = speed;
  j->voice_variant = g_strdup (voice_variant); j->pitch = pitch;
  g_thread_unref (g_thread_new ("wb-read-term", read_thread, j));
}

/* The only place this module touches the network: downloading (and
 * caching) a recorded human pronunciation, opt-in via the
 * use_recorded_audio setting. Cached under <data dir>/audio/<hash of the
 * url><ext>, so a given recording is only ever fetched once. */
gboolean
wb_play_recorded_pronunciation (const char *audio_url)
{
  if (audio_url == NULL || audio_url[0] == '\0')
    return FALSE;

  g_autofree char *dir = g_build_filename (wb_data_dir (), "audio", NULL);
  if (g_mkdir_with_parents (dir, 0755) != 0) {
    log_error ("Could not create audio cache dir %s", dir);
    return FALSE;
  }

  g_autofree char *hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256, audio_url, -1);
  const char *dot = strrchr (audio_url, '.');
  const char *ext = (dot != NULL && strlen (dot) <= 6 &&
                     strpbrk (dot, "/?#") == NULL) ? dot : ".audio";
  g_autofree char *fname = g_strdup_printf ("%s%s", hash, ext);
  g_autofree char *path = g_build_filename (dir, fname, NULL);

  if (!g_file_test (path, G_FILE_TEST_IS_REGULAR)) {
    g_autofree char *tmp = g_strdup_printf ("%s.part", path);
    char *argv[] = { "curl", "-fsSL", "--max-time", "15", "-o", tmp, (char *) audio_url, NULL };
    gboolean nf = FALSE;
    int rc = run_capture (argv, 15000, NULL, &nf);
    GStatBuf st;
    if (nf) {
      log_error ("'curl' not found; cannot download recorded pronunciation");
      g_unlink (tmp);
      return FALSE;
    }
    if (rc != 0 || g_stat (tmp, &st) != 0 || st.st_size == 0) {
      log_error ("Could not download recorded pronunciation from %s", audio_url);
      g_unlink (tmp);
      return FALSE;
    }
    if (g_rename (tmp, path) != 0) {
      log_error ("Could not save recorded pronunciation to %s", path);
      g_unlink (tmp);
      return FALSE;
    }
  }

  if (!wb_espeak_lib_play_file (path)) {
    log_error ("No audio player worked for the recorded pronunciation (tried pw-play, paplay, aplay, ffplay)");
    return FALSE;
  }
  return TRUE;
}

void
wb_get_version_info (const char *app_version)
{
  g_print ("Dictionary %s\n", app_version);
  g_print ("GLib %u.%u.%u\n", glib_major_version, glib_minor_version, glib_micro_version);
  g_print ("SQLite %s\n", sqlite3_libversion ());
  char *argv[] = { "espeak-ng", "--version", NULL };
  char *out = NULL;
  gboolean nf = FALSE;
  if (run_capture (argv, 5000, &out, &nf) == 0 && out) {
    g_autofree char *l = wb_str_strip (out);
    char *nl = strchr (l, '\n');
    if (nl) *nl = '\0';
    g_print ("%s\n", l);
  } else if (wb_espeak_lib_path () != NULL)
    g_print ("espeak-ng command not found; using library %s\n", wb_espeak_lib_path ());
  else
    g_print ("espeak-ng not found\n");
  g_free (out);
}

struct _WbDatabaseInfo {
  GStringChunk *chunk;
  GArray       *sources;    /* WbSourceInfo */
  GArray       *licenses;   /* WbLicenseNotice */
  GArray       *meta;       /* WbMetaEntry, in table order */
  GHashTable   *meta_by_key; /* key -> value, both interned in chunk; for by-key lookup */
};

WbDatabaseInfo *
wb_get_database_info (void)
{
  const char *path = wb_database_path ();
  if (path == NULL)
    return NULL;

  sqlite3 *db = open_db_readonly (path, WB_CACHE_KIB_LITE);
  if (db == NULL)
    return NULL;

  WbDatabaseInfo *info = g_new0 (WbDatabaseInfo, 1);
  info->chunk = g_string_chunk_new (512);
  info->sources = g_array_new (FALSE, FALSE, sizeof (WbSourceInfo));
  info->licenses = g_array_new (FALSE, FALSE, sizeof (WbLicenseNotice));
  info->meta = g_array_new (FALSE, FALSE, sizeof (WbMetaEntry));
  info->meta_by_key = g_hash_table_new (g_str_hash, g_str_equal);

  sqlite3_stmt *st;
  if (sqlite3_prepare_v2 (db, "SELECT name, version, license, url FROM sources ORDER BY name",
                         -1, &st, NULL) == SQLITE_OK) {
    while (sqlite3_step (st) == SQLITE_ROW) {
      WbSourceInfo s;
      s.name    = g_string_chunk_insert_const (info->chunk, col_text (st, 0));
      s.version = g_string_chunk_insert_const (info->chunk, col_text (st, 1));
      s.license = g_string_chunk_insert_const (info->chunk, col_text (st, 2));
      s.url     = g_string_chunk_insert_const (info->chunk, col_text (st, 3));
      g_array_append_val (info->sources, s);
    }
    sqlite3_finalize (st);
  }

  if (sqlite3_prepare_v2 (db, "SELECT name, text FROM license_notice ORDER BY name",
                         -1, &st, NULL) == SQLITE_OK) {
    while (sqlite3_step (st) == SQLITE_ROW) {
      WbLicenseNotice l = {
        .name = g_string_chunk_insert_const (info->chunk, col_text (st, 0)),
        .text = g_string_chunk_insert_const (info->chunk, col_text (st, 1)),
      };
      g_array_append_val (info->licenses, l);
    }
    sqlite3_finalize (st);
  }

  if (sqlite3_prepare_v2 (db, "SELECT key, value FROM meta ORDER BY key", -1, &st, NULL) == SQLITE_OK) {
    while (sqlite3_step (st) == SQLITE_ROW) {
      const char *k = col_text (st, 0), *v = col_text (st, 1);
      if (!k[0])
        continue;
      WbMetaEntry e = { .key   = g_string_chunk_insert_const (info->chunk, k),
                        .value = g_string_chunk_insert_const (info->chunk, v) };
      g_array_append_val (info->meta, e);
      g_hash_table_insert (info->meta_by_key, (gpointer) e.key, (gpointer) e.value);
    }
    sqlite3_finalize (st);
  }

  sqlite3_close (db);
  return info;
}

void
wb_database_info_free (WbDatabaseInfo *info)
{
  if (info == NULL)
    return;
  g_array_unref (info->sources);
  g_array_unref (info->licenses);
  g_array_unref (info->meta);
  g_hash_table_unref (info->meta_by_key);
  g_string_chunk_free (info->chunk);
  g_free (info);
}

const WbSourceInfo *
wb_database_info_get_sources (const WbDatabaseInfo *info, guint *n_out)
{
  if (n_out) *n_out = info ? info->sources->len : 0;
  return info ? (const WbSourceInfo *) info->sources->data : NULL;
}

const WbLicenseNotice *
wb_database_info_get_licenses (const WbDatabaseInfo *info, guint *n_out)
{
  if (n_out) *n_out = info ? info->licenses->len : 0;
  return info ? (const WbLicenseNotice *) info->licenses->data : NULL;
}

const WbMetaEntry *
wb_database_info_get_meta_entries (const WbDatabaseInfo *info, guint *n_out)
{
  if (n_out) *n_out = info ? info->meta->len : 0;
  return info ? (const WbMetaEntry *) info->meta->data : NULL;
}

const char *
wb_database_info_get_meta (const WbDatabaseInfo *info, const char *key)
{
  return info ? g_hash_table_lookup (info->meta_by_key, key) : NULL;
}

typedef struct { guint32 orig, fold; } WordEntry;

struct _WbWordlist {
  char      *arena;
  gsize      arena_len;
  WordEntry *words;
  guint      n;
};

/* Owns a copy of the database path: the worker thread outlives nothing it
 * does not own, so it must not dereference the WbWordnet (the window may free
 * it, e.g. at shutdown, while the word list is still loading). */
typedef struct { char *path; WbWordlistCallback cb; gpointer data; } LoadJob;

static int
entry_cmp_fold (const WordEntry *a, const WordEntry *b, gpointer arena)
{
  int c = strcmp ((char *) arena + a->fold, (char *) arena + b->fold);
  if (c) return c;
  return (a->orig > b->orig) - (a->orig < b->orig);
}

static gpointer
wordlist_thread (gpointer data)
{
  LoadJob *job = data;
  WbWordlist *l = g_new0 (WbWordlist, 1);
  l->arena = g_malloc (1);
  l->arena[0] = '\0';
  l->arena_len = 1;

  sqlite3 *db = open_db_readonly (job->path, WB_CACHE_KIB_LITE);
  if (db) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2 (db,
          "SELECT DISTINCT term FROM suggest WHERE kind = 'lemma'", -1, &st, NULL) == SQLITE_OK) {
      GByteArray *buf = g_byte_array_new ();
      g_byte_array_append (buf, (guint8 *) "", 1);
      GArray *ents = g_array_new (FALSE, FALSE, sizeof (WordEntry));
      GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

      while (sqlite3_step (st) == SQLITE_ROW) {
        const char *w = col_text (st, 0);
        if (w[0] == '\0' || g_hash_table_contains (seen, w))
          continue;
        g_hash_table_add (seen, g_strdup (w));
        WordEntry e;
        e.orig = buf->len;
        g_byte_array_append (buf, (const guint8 *) w, strlen (w) + 1);
        g_autofree char *f = g_utf8_casefold (w, -1);
        if (strcmp (f, w) == 0)
          e.fold = e.orig;
        else {
          e.fold = buf->len;
          g_byte_array_append (buf, (const guint8 *) f, strlen (f) + 1);
        }
        g_array_append_val (ents, e);
      }
      sqlite3_finalize (st);
      g_hash_table_destroy (seen);

      gsize blen = buf->len;
      g_free (l->arena);
      l->arena = (char *) g_byte_array_free (buf, FALSE);
      l->arena_len = blen;
      l->n = ents->len;
      l->words = (WordEntry *) g_array_free (ents, FALSE);
      wb_sort_array (l->words, l->n, sizeof (WordEntry),
                    (GCompareDataFunc) entry_cmp_fold, l->arena);
    }
    sqlite3_close (db);
  }
  if (l->n == 0)
    log_error ("Failed to load Krumeto word list");
  else
    log_info ("Loaded %u words for completion", l->n);

  wb_release_memory ();   /* drop the temporary dedup table / SQLite cache */
  job->cb (l, job->data);
  g_free (job->path);
  g_free (job);
  return NULL;
}

void
wb_wordlist_load_async (WbWordnet *wn, WbWordlistCallback cb, gpointer user_data)
{
  LoadJob *j = g_new0 (LoadJob, 1);
  j->path = g_strdup (wn->path); j->cb = cb; j->data = user_data;
  g_thread_unref (g_thread_new ("wb-wordlist", wordlist_thread, j));
}

/* Up to N distinct random headwords, picked straight from the database by id.
 * Returns a NULL-terminated vector (g_strfreev), never NULL. */
char **
wb_wordnet_random_lemmas (WbWordnet *wn, guint n)
{
  GPtrArray *out = g_ptr_array_new ();
  if (wn == NULL || n == 0)
    goto done;

  g_mutex_lock (&wn->lock);

  if (wn->id_max == 0) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2 (wn->db, "SELECT MIN(id), MAX(id) FROM entries", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step (st) == SQLITE_ROW) {
      wn->id_min = sqlite3_column_int64 (st, 0);
      wn->id_max = sqlite3_column_int64 (st, 1);
    }
    sqlite3_finalize (st);
  }

  if (wn->id_max >= wn->id_min && wn->id_max > 0) {
    guint attempts = 0;
    while (out->len < n && attempts++ < n * 6 + 6) {
      gint64 span = wn->id_max - wn->id_min + 1;
      gint64 id = wn->id_min + (gint64) (g_random_double () * (double) span);
      sqlite3_reset (wn->st_random);
      sqlite3_bind_int64 (wn->st_random, 1, id);
      if (sqlite3_step (wn->st_random) != SQLITE_ROW)
        continue;
      const char *lemma = col_text (wn->st_random, 0);
      if (lemma[0] == '\0')
        continue;
      gboolean dup = FALSE;
      for (guint i = 0; i < out->len && !dup; i++)
        dup = g_ascii_strcasecmp (g_ptr_array_index (out, i), lemma) == 0;
      if (!dup)
        g_ptr_array_add (out, g_strdup (lemma));
    }
    sqlite3_reset (wn->st_random);
  }
  g_mutex_unlock (&wn->lock);

done:
  g_ptr_array_add (out, NULL);
  return (char **) g_ptr_array_free (out, FALSE);
}

guint wb_wordlist_size (const WbWordlist *l) { return l->n; }

const char *
wb_wordlist_get (const WbWordlist *l, guint i)
{
  return (l && i < l->n) ? l->arena + l->words[i].orig : NULL;
}

void
wb_wordlist_free (WbWordlist *l)
{
  if (l == NULL) return;
  g_free (l->arena);
  g_free (l->words);
  g_free (l);
}

typedef struct {
  int spaces, nonalnum, nchars;
  const char *fold;
  const char *word;
} Ranked;

static int
rank_cmp (const Ranked *a, const Ranked *b)
{
  if (a->spaces != b->spaces) return a->spaces < b->spaces ? -1 : 1;
  if (a->nonalnum != b->nonalnum) return a->nonalnum < b->nonalnum ? -1 : 1;
  if (a->nchars != b->nchars) return a->nchars < b->nchars ? -1 : 1;
  return strcmp (a->fold, b->fold);
}

char **
wb_wordlist_complete (const WbWordlist *l, const char *text, int limit)
{
  if (limit <= 0 || l == NULL || l->n == 0)
    return g_new0 (char *, 1);

  const char *typed = text;
  while (*typed && g_unichar_isspace (g_utf8_get_char (typed)))
    typed = g_utf8_next_char (typed);
  if (*typed == '\0')
    return g_new0 (char *, 1);

  g_autofree char *search = g_utf8_casefold (typed, -1);
  gsize slen = strlen (search);
  gsize typed_bytes_cmp_len = strlen (typed);

  guint lo = 0, hi = l->n;
  while (lo < hi) {
    guint mid = lo + (hi - lo) / 2;
    if (strcmp (l->arena + l->words[mid].fold, search) < 0) lo = mid + 1;
    else hi = mid;
  }

  Ranked *top = g_new (Ranked, limit + 1);
  int ntop = 0;

  guint i = lo;
  while (i < l->n) {
    const char *fold = l->arena + l->words[i].fold;
    if (strncmp (fold, search, slen) != 0)
      break;

    const char *rep = l->arena + l->words[i].orig;
    guint j = i + 1;
    while (j < l->n && strcmp (l->arena + l->words[j].fold, fold) == 0) {
      const char *cand = l->arena + l->words[j].orig;
      if (strncmp (rep, typed, typed_bytes_cmp_len) != 0 &&
          strncmp (cand, typed, typed_bytes_cmp_len) == 0)
        rep = cand;
      j++;
    }

    Ranked r = { 0, 0, 0, fold, rep };
    for (const char *p = fold + slen; *p; p = g_utf8_next_char (p)) {
      gunichar c = g_utf8_get_char (p);
      if (c == ' ') r.spaces++;
      if (!g_unichar_isalnum (c)) r.nonalnum++;
      r.nchars++;
    }

    if (ntop < limit || rank_cmp (&r, &top[ntop - 1]) < 0) {
      int pos = ntop < limit ? ntop : limit - 1;
      while (pos > 0 && rank_cmp (&r, &top[pos - 1]) < 0) {
        top[pos] = top[pos - 1];
        pos--;
      }
      top[pos] = r;
      if (ntop < limit)
        ntop++;
    }
    i = j;
  }

  char **out = g_new (char *, ntop + 1);
  for (int k = 0; k < ntop; k++)
    out[k] = g_strdup (top[k].word);
  out[ntop] = NULL;
  g_free (top);
  return out;
}
