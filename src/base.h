#pragma once

#include <glib.h>
#include "constants.h"

G_BEGIN_DECLS

void  wb_create_required_dirs (void);

char *wb_clean_search_terms (const char *search_term);

char *wb_ipa_to_espeak (const char *ipa);

typedef struct {
  const char *ipa;
  gboolean    is_fallback;
} WbPronunciation;

typedef struct {
  const char *name;
  const char *definition;
  const char *const *examples;
  const char *const *syn;
  const char *const *ant;
  const char *const *sim;
  const char *const *also_sees;
  const WbPronunciation *pron;
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

void  wb_read_term       (const char *text, int speed, const char *accent, const char *ipa);
void  wb_read_term_async (const char *text, int speed, const char *accent, const char *ipa);

void  wb_get_version_info (const char *app_version);

G_END_DECLS
