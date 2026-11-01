#pragma once

#include <glib.h>
#include "constants.h"

G_BEGIN_DECLS

void  wb_create_required_dirs (void);

char *wb_clean_search_terms (const char *search_term);

char *wb_ipa_to_espeak (const char *ipa);

typedef struct {
  const char *ipa;            /* "" when only a recording exists */
  gboolean    is_fallback;
  const char *audio_url;      /* recorded pronunciation, or NULL */
  const char *audio_credit;
  const char *audio_license;
} WbPronunciation;

typedef struct {
  const char *form;             /* forms.form, e.g. "running"               */
  const char *const *tags;      /* forms.tags (JSON array), e.g. ["participle"] */
} WbInflectedForm;

typedef struct {
  const char *pattern;          /* collocations.pattern, e.g. "run * errand" */
  int         count;
} WbCollocation;

typedef struct {
  const char *name;
  const char *definition;
  const char *const *examples;
  const char *const *syn;         /* links.kind = 'syn'        */
  const char *const *ant;         /* links.kind = 'ant' (+ lemma_rel fallback) */
  const char *const *broader;     /* links.kind = 'hypernym'   */
  const char *const *narrower;    /* links.kind = 'hyponym'    */
  const char *const *coordinate;  /* links.kind = 'coordinate' */
  const char *const *derived;     /* links.kind = 'derived'    */
  const char *const *related;     /* links.kind = 'related'    */
  const char *const *pertains;    /* lemma_rel 'pertainym'  */
  const char *const *participle;  /* lemma_rel 'participle' */
  const WbPronunciation *pron;

  /* sense level */
  const char *parent_definition;  /* senses.parent_gloss, NULL if top-level */
  const char *const *tags;        /* senses.tags (JSON array), e.g. "informal" */

  /* entry level (identical on every sense of the same entry) */
  const char *cefr;               /* COALESCE(cefr_pos, cefr)  */
  double      zipf;               /* < 0 when unknown          */
  const char *freq_bucket;
  const char *hyphenation;
  const char *etymology;
  const char *usage_notes;
  const char *wiki_url;
  const char *countable;          /* countable | uncountable | both */
  gboolean    transitive;

  const WbInflectedForm *word_family;   /* forms, excluding the headword itself */
  guint                  n_word_family;
  const WbCollocation   *collocations;  /* collocations, highest count first    */
  guint                  n_collocations;
  const char *const     *extra_examples; /* example_top + sentences, curated corpus examples */
} WbSynsetData;

typedef struct _WbDefinition WbDefinition;

const char *wb_definition_get_term   (const WbDefinition *d);
gboolean    wb_definition_has_result (const WbDefinition *d);
const WbSynsetData *wb_definition_get_synsets (const WbDefinition *d, WbPos pos, guint *n_out);
void        wb_definition_free       (WbDefinition *d);

typedef struct {
  const WbPronunciation *pronunciation;
  const WbSynsetData   **synsets;
  guint                  n_synsets;
} WbPronunciationGroup;

typedef struct {
  const char           *lemma;
  WbPronunciationGroup *pronunciation_groups;
  guint                 n_pronunciation_groups;
} WbLemmaGroup;

typedef struct {
  WbLemmaGroup *groups;
  guint         n_groups;
} WbLemmaGroups;

WbLemmaGroups *wb_group_synsets_by_lemma (const WbSynsetData *synsets, guint n_synsets);
void           wb_lemma_groups_free      (WbLemmaGroups *g);

typedef struct _WbWordnet WbWordnet;

WbWordnet *wb_get_wn_instance (void);
WbWordnet *wb_get_wn_instance_lite (void);   /* tiny page cache: popup / background */
void       wb_release_memory  (void);        /* sqlite cache + malloc_trim */
char     **wb_wordnet_random_lemmas (WbWordnet *wn, guint n);  /* g_strfreev */
void       wb_wordnet_free    (WbWordnet *wn);

WbDefinition *wb_get_definition   (const char *term, WbWordnet *wn, const char *accent);
WbDefinition *wb_fetch_definition (const char *term, WbWordnet *wn, const char *accent);
WbDefinition *wb_format_output    (const char *text, WbWordnet *wn, const char *accent);

typedef struct _WbWordlist WbWordlist;

typedef void (*WbWordlistCallback) (WbWordlist *list, gpointer user_data);

void   wb_wordlist_load_async (WbWordnet *wn, WbWordlistCallback cb, gpointer user_data);
guint  wb_wordlist_size       (const WbWordlist *l);
const char *wb_wordlist_get   (const WbWordlist *l, guint i);
void   wb_wordlist_free       (WbWordlist *l);

char **wb_wordlist_complete   (const WbWordlist *l, const char *text, int limit);

char *wb_get_pronunciation (const char *term, const char *accent);

/* VOICE_VARIANT is appended straight to the accent's base voice id (e.g.
 * "en-us" + "+f3" -> "en-us+f3"); pass "" for the plain default voice.
 * PITCH is 0-99 (espeak-ng's own scale, 50 = unchanged). */
void  wb_read_term       (const char *text, int speed, const char *accent, const char *ipa,
                          const char *voice_variant, int pitch);
void  wb_read_term_async (const char *text, int speed, const char *accent, const char *ipa,
                          const char *voice_variant, int pitch);

/* Plays a recorded human pronunciation (pron.audio_url), downloading and
 * caching it under the data directory first if needed. Returns FALSE (with
 * nothing played) if there's no network, the download fails, or no local
 * player works - callers should fall back to wb_read_term() in that case. */
gboolean wb_play_recorded_pronunciation (const char *audio_url);

void  wb_get_version_info (const char *app_version);

/* Dataset provenance and licensing (sources, license_notice, meta) - used
 * by the About dialog. A short-lived, separate read: call once, not on
 * every lookup. */
typedef struct {
  const char *name, *version, *license, *url;
} WbSourceInfo;

typedef struct {
  const char *name, *text;
} WbLicenseNotice;

typedef struct {
  const char *key, *value;
} WbMetaEntry;

typedef struct _WbDatabaseInfo WbDatabaseInfo;

WbDatabaseInfo *wb_get_database_info      (void);
void            wb_database_info_free     (WbDatabaseInfo *info);
const WbSourceInfo    *wb_database_info_get_sources      (const WbDatabaseInfo *info, guint *n_out);
const WbLicenseNotice *wb_database_info_get_licenses     (const WbDatabaseInfo *info, guint *n_out);
const WbMetaEntry      *wb_database_info_get_meta_entries (const WbDatabaseInfo *info, guint *n_out);
const char              *wb_database_info_get_meta (const WbDatabaseInfo *info, const char *key);

G_END_DECLS
