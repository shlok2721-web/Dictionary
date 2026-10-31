#define _GNU_SOURCE
#include "base.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <poll.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "utils.h"

void
wb_create_required_dirs (void)
{
  g_autofree char *wn_dir = g_strdup_printf ("%s/wn-%s", wb_data_dir (), WN_FILE_VERSION);
  g_mkdir_with_parents (wb_config_dir (), 0755);
  g_mkdir_with_parents (wb_data_dir (), 0755);
  g_mkdir_with_parents (wn_dir, 0755);
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

typedef struct { gunichar *c; int n; } UStr;

static UStr
ustr_new (const char *s)
{
  UStr u;
  glong n;
  u.c = g_utf8_to_ucs4_fast (s, -1, &n);
  u.n = (int) n;
  return u;
}

static void
longest_match (const UStr *a, int alo, int ahi, const UStr *b, int blo, int bhi,
               int *buf, int *bi, int *bj, int *bk)
{
  int *j2len = buf, *newj2len = buf + (b->n + 1);
  int besti = alo, bestj = blo, bestsize = 0;

  memset (j2len, 0, sizeof (int) * (b->n + 1));
  for (int i = alo; i < ahi; i++) {
    memset (newj2len, 0, sizeof (int) * (b->n + 1));
    for (int j = blo; j < bhi; j++) {
      if (a->c[i] != b->c[j])
        continue;
      int k = j2len[j] + 1;
      newj2len[j + 1] = k;
      if (k > bestsize) {
        besti = i - k + 1;
        bestj = j - k + 1;
        bestsize = k;
      }
    }
    int *t = j2len; j2len = newj2len; newj2len = t;
  }
  *bi = besti; *bj = bestj; *bk = bestsize;
}

static double
seq_ratio (const UStr *a, const UStr *b)
{
  int total = a->n + b->n;
  if (total == 0)
    return 1.0;

  int *buf = g_new (int, 2 * (b->n + 1));
  int stack[256][4];
  int sp = 0, matches = 0;
  stack[sp][0] = 0; stack[sp][1] = a->n; stack[sp][2] = 0; stack[sp][3] = b->n; sp++;

  while (sp > 0) {
    sp--;
    int alo = stack[sp][0], ahi = stack[sp][1], blo = stack[sp][2], bhi = stack[sp][3];
    int i, j, k;
    longest_match (a, alo, ahi, b, blo, bhi, buf, &i, &j, &k);
    if (k == 0)
      continue;
    matches += k;
    if (alo < i && blo < j && sp < 254) {
      stack[sp][0] = alo; stack[sp][1] = i; stack[sp][2] = blo; stack[sp][3] = j; sp++;
    }
    if (i + k < ahi && j + k < bhi && sp < 254) {
      stack[sp][0] = i + k; stack[sp][1] = ahi; stack[sp][2] = j + k; stack[sp][3] = bhi; sp++;
    }
  }
  g_free (buf);
  return 2.0 * matches / total;
}

static int
close_match_index (const char *term, char *const *lemmas, guint n, double cutoff)
{
  UStr b = ustr_new (term);
  int best = -1;
  double best_score = 0;

  for (guint i = 0; i < n; i++) {
    UStr a = ustr_new (lemmas[i]);
    int mn = MIN (a.n, b.n), tot = a.n + b.n;
    double rq = tot ? 2.0 * mn / tot : 1.0;
    if (rq >= cutoff) {
      double r = seq_ratio (&a, &b);
      if (r >= cutoff &&
          (best < 0 || r > best_score ||
           (r == best_score && strcmp (lemmas[i], lemmas[best]) > 0))) {
        best = (int) i;
        best_score = r;
      }
    }
    g_free (a.c);
  }
  g_free (b.c);
  return best;
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

struct _WbWordnet {
  sqlite3      *db;
  GMutex        lock;
  gint64        lexicon;
  gint64        rt_antonym, rt_similar, rt_also;
  sqlite3_stmt *st_synsets, *st_members, *st_def, *st_examples,
               *st_pron, *st_ant, *st_rel;
  char         *path;
};

static const char SQL_SYNSETS[] =
  "WITH wf(s) AS (VALUES (?1)),"
  " me(rowid) AS (SELECT f.entry_rowid FROM forms f"
  "                WHERE f.form IN wf OR f.normalized_form IN wf)"
  "SELECT ss.rowid, ss.pos FROM synsets ss"
  "  JOIN (SELECT _s.synset_rowid, _s.entry_rank, _s.entry_rowid FROM senses _s"
  "         WHERE _s.entry_rowid IN"
  "           (SELECT rowid FROM me"
  "            UNION SELECT idx.entry_rowid FROM me _me"
  "                    JOIN entry_index _idx ON _idx.entry_rowid = _me.rowid"
  "                    JOIN entry_index idx ON idx.lemma = _idx.lemma)) s"
  "    ON s.synset_rowid = ss.rowid"
  " WHERE ss.lexicon_rowid = ?2"
  " ORDER BY ss.pos, s.entry_rank, s.entry_rowid, ss.rowid";

static const char SQL_MEMBERS[] =
  "SELECT s.entry_rowid, f.rowid, f.form FROM senses s"
  "  JOIN forms f ON f.entry_rowid = s.entry_rowid AND f.rank = 0"
  " WHERE s.synset_rowid = ?1 AND s.lexicon_rowid = ?2"
  " ORDER BY s.synset_rank, s.rowid";

static const char SQL_DEF[] =
  "SELECT definition FROM definitions"
  " WHERE synset_rowid = ?1 AND lexicon_rowid = ?2 ORDER BY rowid LIMIT 1";

static const char SQL_EXAMPLES[] =
  "SELECT example FROM synset_examples"
  " WHERE synset_rowid = ?1 AND lexicon_rowid = ?2 ORDER BY rowid";

static const char SQL_PRON[] =
  "SELECT value, variety FROM pronunciations"
  " WHERE form_rowid = ?1 AND lexicon_rowid = ?2 ORDER BY rowid";

static const char SQL_ANT[] =
  "SELECT f.form FROM senses s"
  "  JOIN sense_relations sr ON sr.source_rowid = s.rowid"
  "  JOIN senses t ON t.rowid = sr.target_rowid"
  "  JOIN forms f ON f.entry_rowid = t.entry_rowid AND f.rank = 0"
  " WHERE s.synset_rowid = ?1 AND s.lexicon_rowid = ?2"
  "   AND sr.type_rowid = ?3 AND sr.lexicon_rowid = ?2"
  " ORDER BY s.synset_rank, s.rowid, sr.rowid";

static const char SQL_REL[] =
  "SELECT sr.type_rowid, f.form FROM synset_relations sr"
  "  JOIN senses s ON s.synset_rowid = sr.target_rowid AND s.lexicon_rowid = ?2"
  "  JOIN forms f ON f.entry_rowid = s.entry_rowid AND f.rank = 0"
  " WHERE sr.source_rowid = ?1 AND sr.lexicon_rowid = ?2"
  "   AND sr.type_rowid IN (?3, ?4)"
  " ORDER BY sr.rowid, s.synset_rank, s.rowid";

static gint64
lookup_relation_type (sqlite3 *db, const char *name)
{
  sqlite3_stmt *st;
  gint64 id = -1;
  if (sqlite3_prepare_v2 (db, "SELECT rowid FROM relation_types WHERE type = ?1", -1, &st, NULL) == SQLITE_OK) {
    sqlite3_bind_text (st, 1, name, -1, SQLITE_STATIC);
    if (sqlite3_step (st) == SQLITE_ROW)
      id = sqlite3_column_int64 (st, 0);
    sqlite3_finalize (st);
  }
  return id;
}

static sqlite3 *
open_db_readonly (const char *path)
{
  sqlite3 *db = NULL;
  if (sqlite3_open_v2 (path, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK) {
    log_error ("WordNet initialization failed: %s", db ? sqlite3_errmsg (db) : "out of memory");
    sqlite3_close (db);
    return NULL;
  }
  sqlite3_exec (db, "PRAGMA query_only=1; PRAGMA cache_size=-2048;"
                    "PRAGMA mmap_size=0; PRAGMA temp_store=MEMORY;",
                NULL, NULL, NULL);
  return db;
}

void
wb_wordnet_free (WbWordnet *wn)
{
  if (wn == NULL)
    return;
  sqlite3_stmt **all[] = { &wn->st_synsets, &wn->st_members, &wn->st_def,
                           &wn->st_examples, &wn->st_pron, &wn->st_ant, &wn->st_rel };
  for (gsize i = 0; i < G_N_ELEMENTS (all); i++)
    sqlite3_finalize (*all[i]);
  sqlite3_close (wn->db);
  g_mutex_clear (&wn->lock);
  g_free (wn->path);
  g_free (wn);
}

WbWordnet *
wb_get_wn_instance (void)
{
  log_info ("Initializing WordNet...");
  WbWordnet *wn = g_new0 (WbWordnet, 1);
  g_mutex_init (&wn->lock);
  wn->path = g_strdup_printf ("%s/wn-%s/wn.db", wb_data_dir (), WN_FILE_VERSION);

  wn->db = open_db_readonly (wn->path);
  if (wn->db == NULL)
    goto fail;

  sqlite3_stmt *st;
  if (sqlite3_prepare_v2 (wn->db, "SELECT rowid FROM lexicons WHERE specifier = ?1", -1, &st, NULL) != SQLITE_OK) {
    log_error ("WordNet initialization failed: %s", sqlite3_errmsg (wn->db));
    goto fail;
  }
  sqlite3_bind_text (st, 1, WB_WN_DB_VERSION, -1, SQLITE_STATIC);
  gboolean have = sqlite3_step (st) == SQLITE_ROW;
  if (have)
    wn->lexicon = sqlite3_column_int64 (st, 0);
  sqlite3_finalize (st);
  if (!have) {
    log_error ("WordNet initialization failed: no such lexicon: %s", WB_WN_DB_VERSION);
    goto fail;
  }

  wn->rt_antonym = lookup_relation_type (wn->db, "antonym");
  wn->rt_similar = lookup_relation_type (wn->db, "similar");
  wn->rt_also    = lookup_relation_type (wn->db, "also");

  struct { sqlite3_stmt **st; const char *sql; } prep[] = {
    { &wn->st_synsets, SQL_SYNSETS }, { &wn->st_members,  SQL_MEMBERS },
    { &wn->st_def,     SQL_DEF },     { &wn->st_examples, SQL_EXAMPLES },
    { &wn->st_pron,    SQL_PRON },    { &wn->st_ant,      SQL_ANT },
    { &wn->st_rel,     SQL_REL },
  };
  for (gsize i = 0; i < G_N_ELEMENTS (prep); i++)
    if (sqlite3_prepare_v3 (wn->db, prep[i].sql, -1, SQLITE_PREPARE_PERSISTENT,
                            prep[i].st, NULL) != SQLITE_OK) {
      log_error ("WordNet initialization failed: %s", sqlite3_errmsg (wn->db));
      goto fail;
    }

  log_info ("WordNet instance (%s) created and ready.", WB_WN_DB_VERSION);
  return wn;

fail:
  wb_wordnet_free (wn);
  return NULL;
}

typedef struct {
  gint64 entry_rowid, form_rowid;
  char  *form;
} Member;

static const char *
col_text (sqlite3_stmt *st, int i)
{
  const char *t = (const char *) sqlite3_column_text (st, i);
  return t ? t : "";
}

static const WbPronunciation *
pick_pronunciation (WbWordnet *wn, WbDefinition *d, gint64 form_rowid, const char *accent)
{
  sqlite3_stmt *st = wn->st_pron;
  sqlite3_reset (st);
  sqlite3_bind_int64 (st, 1, form_rowid);
  sqlite3_bind_int64 (st, 2, wn->lexicon);

  g_autofree char *want_s = wb_str_strip (accent ? accent : "");
  g_autofree char *want = g_utf8_casefold (want_s, -1);
  char *first = NULL, *chosen = NULL;

  while (sqlite3_step (st) == SQLITE_ROW) {
    const char *value = col_text (st, 0);
    if (first == NULL)
      first = g_string_chunk_insert_const (d->chunk, value);
    g_autofree char *v1 = wb_str_strip (col_text (st, 1));
    g_autofree char *v2 = g_utf8_casefold (v1, -1);
    if (v2[0] != '\0' && strcmp (v2, want) == 0) {
      chosen = g_string_chunk_insert_const (d->chunk, value);
      break;
    }
  }
  sqlite3_reset (st);
  if (chosen == NULL)
    chosen = first;
  if (chosen == NULL)
    return NULL;

  WbPronunciation *p = g_new0 (WbPronunciation, 1);
  p->ipa = chosen;
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

WbDefinition *
wb_get_definition (const char *term, WbWordnet *wn, const char *accent)
{
  WbDefinition *d = definition_new (term);
  g_autofree char *lower = g_utf8_strdown (term, -1);

  g_mutex_lock (&wn->lock);

  GArray *syn_ids = g_array_new (FALSE, FALSE, sizeof (gint64));
  GArray *syn_pos = g_array_new (FALSE, FALSE, sizeof (char));
  g_autofree char *norm = normalize_form (lower);
  const char *attempts[2] = { lower, norm };
  for (int att = 0; att < 2 && syn_ids->len == 0; att++) {
    if (att == 1 && strcmp (norm, lower) == 0)
      break;
    sqlite3_stmt *st = wn->st_synsets;
    sqlite3_reset (st);
    sqlite3_bind_text (st, 1, attempts[att], -1, SQLITE_STATIC);
    sqlite3_bind_int64 (st, 2, wn->lexicon);
    GHashTable *seen = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, NULL);
    while (sqlite3_step (st) == SQLITE_ROW) {
      gint64 id = sqlite3_column_int64 (st, 0);
      if (g_hash_table_contains (seen, &id))
        continue;
      gint64 *key = g_memdup2 (&id, sizeof id);
      g_hash_table_add (seen, key);
      g_array_append_val (syn_ids, id);
      const char *pos = col_text (st, 1);
      char pc = pos[0];
      g_array_append_val (syn_pos, pc);
    }
    sqlite3_reset (st);
    g_hash_table_destroy (seen);
  }

  if (syn_ids->len == 0) {
    g_mutex_unlock (&wn->lock);
    g_array_unref (syn_ids);
    g_array_unref (syn_pos);
    return d;
  }

  d->found = TRUE;
  char *first_match = NULL;

  for (guint si = 0; si < syn_ids->len; si++) {
    gint64 sid = g_array_index (syn_ids, gint64, si);
    char tag[2] = { g_array_index (syn_pos, char, si), 0 };
    WbPos pos;
    if (!wb_pos_from_tag (tag, &pos))
      log_warning ("Unknown POS tag encountered: %s for term '%s'", tag, term);

    GArray *members = g_array_new (FALSE, FALSE, sizeof (Member));
    GPtrArray *raw_lemmas = g_ptr_array_new ();
    sqlite3_stmt *st = wn->st_members;
    sqlite3_reset (st);
    sqlite3_bind_int64 (st, 1, sid);
    sqlite3_bind_int64 (st, 2, wn->lexicon);
    while (sqlite3_step (st) == SQLITE_ROW) {
      Member m = { sqlite3_column_int64 (st, 0), sqlite3_column_int64 (st, 1),
                   g_strdup (col_text (st, 2)) };
      g_array_append_val (members, m);
      g_ptr_array_add (raw_lemmas, m.form);
    }
    sqlite3_reset (st);

    if (members->len == 0) {
      g_array_unref (members);
      g_ptr_array_unref (raw_lemmas);
      continue;
    }

    g_autofree char *term_lower = g_utf8_strdown (term, -1);
    g_autofree char *nterm = wb_str_strip (term_lower);
    g_autofree char *matched = NULL;
    for (guint i = 0; i < raw_lemmas->len && matched == NULL; i++) {
      g_autofree char *nl = normalize_lemma (g_ptr_array_index (raw_lemmas, i));
      g_autofree char *nll = g_utf8_strdown (nl, -1);
      if (strcmp (nll, nterm) == 0)
        matched = g_steal_pointer (&nl);
    }
    if (matched == NULL) {
      int idx = close_match_index (term, (char *const *) raw_lemmas->pdata, raw_lemmas->len, 0.8);
      matched = normalize_lemma (idx >= 0 ? g_ptr_array_index (raw_lemmas, idx)
                                          : g_ptr_array_index (raw_lemmas, 0));
    }
    g_autofree char *matched_lower = g_utf8_strdown (matched, -1);
    if (first_match == NULL)
      first_match = g_string_chunk_insert_const (d->chunk, matched);

    const WbPronunciation *pron = NULL;
    for (guint i = 0; i < members->len; i++) {
      Member *m = &g_array_index (members, Member, i);
      g_autofree char *nl = normalize_lemma (m->form);
      g_autofree char *nll = g_utf8_strdown (nl, -1);
      if (strcmp (nll, matched_lower) == 0) {
        pron = pick_pronunciation (wn, d, m->form_rowid, accent);
        break;
      }
    }

    GPtrArray *syn = g_ptr_array_new ();
    for (guint i = 0; i < members->len; i++) {
      g_autofree char *nl = normalize_lemma (g_array_index (members, Member, i).form);
      g_autofree char *nll = g_utf8_strdown (nl, -1);
      if (strcmp (nll, matched_lower) != 0)
        g_ptr_array_add (syn, g_string_chunk_insert_const (d->chunk, nl));
    }

    GPtrArray *ant = g_ptr_array_new ();
    if (wn->rt_antonym >= 0) {
      st = wn->st_ant;
      sqlite3_reset (st);
      sqlite3_bind_int64 (st, 1, sid);
      sqlite3_bind_int64 (st, 2, wn->lexicon);
      sqlite3_bind_int64 (st, 3, wn->rt_antonym);
      while (sqlite3_step (st) == SQLITE_ROW) {
        g_autofree char *nl = normalize_lemma (col_text (st, 0));
        char *interned = g_string_chunk_insert_const (d->chunk, nl);
        if (!vec_contains (ant, interned))
          g_ptr_array_add (ant, interned);
      }
      sqlite3_reset (st);
    }

    GPtrArray *sim = g_ptr_array_new (), *also = g_ptr_array_new ();
    st = wn->st_rel;
    sqlite3_reset (st);
    sqlite3_bind_int64 (st, 1, sid);
    sqlite3_bind_int64 (st, 2, wn->lexicon);
    sqlite3_bind_int64 (st, 3, wn->rt_similar);
    sqlite3_bind_int64 (st, 4, wn->rt_also);
    while (sqlite3_step (st) == SQLITE_ROW) {
      gint64 type = sqlite3_column_int64 (st, 0);
      GPtrArray *target = (type == wn->rt_similar) ? sim : also;
      g_autofree char *nl = normalize_lemma (col_text (st, 1));
      g_autofree char *nll = g_utf8_strdown (nl, -1);
      if (strcmp (nll, matched_lower) == 0)
        continue;
      char *interned = g_string_chunk_insert_const (d->chunk, nl);
      if (!vec_contains (target, interned))
        g_ptr_array_add (target, interned);
    }
    sqlite3_reset (st);

    st = wn->st_def;
    sqlite3_reset (st);
    sqlite3_bind_int64 (st, 1, sid);
    sqlite3_bind_int64 (st, 2, wn->lexicon);
    const char *def_text = NULL;
    if (sqlite3_step (st) == SQLITE_ROW)
      def_text = col_text (st, 0);
    char *definition = g_string_chunk_insert (d->chunk,
        (def_text && def_text[0]) ? def_text : "No definition available.");
    sqlite3_reset (st);

    GPtrArray *ex = g_ptr_array_new ();
    st = wn->st_examples;
    sqlite3_reset (st);
    sqlite3_bind_int64 (st, 1, sid);
    sqlite3_bind_int64 (st, 2, wn->lexicon);
    while (sqlite3_step (st) == SQLITE_ROW)
      g_ptr_array_add (ex, g_string_chunk_insert (d->chunk, col_text (st, 0)));
    sqlite3_reset (st);

    WbSynsetData sd = {
      .name = g_string_chunk_insert_const (d->chunk, matched),
      .definition = definition,
      .examples = make_vector (d, ex),
      .syn = make_vector (d, syn),
      .ant = make_vector (d, ant),
      .sim = make_vector (d, sim),
      .also_sees = make_vector (d, also),
      .pron = pron,
    };
    if (d->by_pos[pos] == NULL)
      d->by_pos[pos] = g_array_new (FALSE, FALSE, sizeof (WbSynsetData));
    g_array_append_val (d->by_pos[pos], sd);

    g_ptr_array_unref (syn); g_ptr_array_unref (ant); g_ptr_array_unref (sim);
    g_ptr_array_unref (also); g_ptr_array_unref (ex);
    for (guint i = 0; i < members->len; i++)
      g_free (g_array_index (members, Member, i).form);
    g_array_unref (members);
    g_ptr_array_unref (raw_lemmas);
  }
  g_mutex_unlock (&wn->lock);

  g_free (d->term);
  d->term = g_strdup (first_match ? first_match : term);
  g_array_unref (syn_ids);
  g_array_unref (syn_pos);
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
    log_error ("'espeak-ng' command not found. Please install it.");
    return NULL;
  }
  if (rc == -2) {
    log_error ("espeak-ng timed out getting pronunciation for '%s'", term);
    g_free (out);
    return NULL;
  }
  if (rc != 0 || out == NULL) {
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
    for (guint i = 0; a && i < a->len && !needs; i++) {
      WbSynsetData *s = &g_array_index (a, WbSynsetData, i);
      g_autofree char *nf = g_utf8_casefold (s->name, -1);
      if (s->pron == NULL && strcmp (nf, resolved_fold) == 0)
        needs = TRUE;
    }
  }
  if (!needs)
    return d;

  char *ipa = wb_get_pronunciation (d->term, accent);
  if (ipa == NULL)
    return d;

  WbPronunciation *fb = g_new0 (WbPronunciation, 1);
  fb->ipa = g_string_chunk_insert_const (d->chunk, ipa);
  fb->is_fallback = TRUE;
  g_ptr_array_add (d->prons, fb);
  g_free (ipa);

  for (int p = 0; p < WB_POS_COUNT; p++) {
    GArray *a = d->by_pos[p];
    for (guint i = 0; a && i < a->len; i++) {
      WbSynsetData *s = &g_array_index (a, WbSynsetData, i);
      g_autofree char *nf = g_utf8_casefold (s->name, -1);
      if (s->pron == NULL && strcmp (nf, resolved_fold) == 0)
        s->pron = fb;
    }
  }
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

void
wb_read_term (const char *text, int speed, const char *accent, const char *ipa)
{
  g_autofree char *spoken = NULL;
  if (ipa != NULL && ipa[0] != '\0')
    spoken = wb_ipa_to_espeak (ipa);
  const char *voice = (accent && strcmp (accent, "gb") == 0) ? "en-gb" : "en-us";
  g_autofree char *sp = g_strdup_printf ("%d", speed);
  char *argv[] = { "espeak-ng", "-v", (char *) voice, "-s", sp,
                   (char *) (spoken ? spoken : text), NULL };
  gboolean nf = FALSE;
  int rc = run_capture (argv, 10000, NULL, &nf);
  if (nf)
    log_error ("'espeak-ng' command not found. Please install it.");
  else if (rc == -2)
    log_error ("espeak-ng timed out");
  else if (rc != 0)
    log_error ("espeak-ng failed with code %d", rc);
}

typedef struct { char *text, *accent, *ipa; int speed; } ReadJob;

static gpointer
read_thread (gpointer data)
{
  ReadJob *j = data;
  wb_read_term (j->text, j->speed, j->accent, j->ipa);
  g_free (j->text); g_free (j->accent); g_free (j->ipa); g_free (j);
  return NULL;
}

void
wb_read_term_async (const char *text, int speed, const char *accent, const char *ipa)
{
  ReadJob *j = g_new0 (ReadJob, 1);
  j->text = g_strdup (text); j->accent = g_strdup (accent);
  j->ipa = g_strdup (ipa); j->speed = speed;
  g_thread_unref (g_thread_new ("wb-read-term", read_thread, j));
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
  } else
    g_print ("espeak-ng not found\n");
  g_free (out);
}

typedef struct { guint32 orig, fold; } WordEntry;

struct _WbWordlist {
  char      *arena;
  gsize      arena_len;
  WordEntry *words;
  guint      n;
};

typedef struct { WbWordnet *wn; WbWordlistCallback cb; gpointer data; } LoadJob;

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

  sqlite3 *db = open_db_readonly (job->wn->path);
  if (db) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2 (db,
          "SELECT f.form FROM forms f JOIN entries e ON e.rowid = f.entry_rowid"
          " WHERE f.rank = 0 AND e.lexicon_rowid = ?1 ORDER BY f.rowid", -1, &st, NULL) == SQLITE_OK) {
      sqlite3_bind_int64 (st, 1, job->wn->lexicon);
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
    log_error ("Failed to load WordNet word list");
  else
    log_info ("Loaded %u words for completion", l->n);

  job->cb (l, job->data);
  g_free (job);
  return NULL;
}

void
wb_wordlist_load_async (WbWordnet *wn, WbWordlistCallback cb, gpointer user_data)
{
  LoadJob *j = g_new0 (LoadJob, 1);
  j->wn = wn; j->cb = cb; j->data = user_data;
  g_thread_unref (g_thread_new ("wb-wordlist", wordlist_thread, j));
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
