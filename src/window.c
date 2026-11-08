#include "window.h"

#include <glib/gi18n.h>
#include <string.h>

#include "base.h"
#include "constants.h"
#include "database.h"
#include "search_completion.h"
#include "settings.h"
#include "settings_window.h"
#include "utils.h"

#ifndef WB_RES_PATH
#define WB_RES_PATH "/dev/silvanus/dictionary"
#endif

#define LOG_INFO(...)  log_info (__VA_ARGS__)
#define LOG_ERROR(...) log_error (__VA_ARGS__)

static inline gboolean cfg_live_search (void)         { return wb_settings_get_live_search (wb_settings_get ()); }
static inline gboolean cfg_double_click (void)        { return wb_settings_get_double_click (wb_settings_get ()); }
static inline gboolean cfg_auto_paste_on_launch (void){ return wb_settings_get_auto_paste_on_launch (wb_settings_get ()); }
static inline const char *cfg_accent_code (void)
{
  return wb_accent_code (wb_settings_get_pronunciations_accent (wb_settings_get ()));
}
static inline const char *cfg_voice_variant (void)
{
  return wb_voice_variant (wb_settings_get_voice_style (wb_settings_get ()));
}
static inline int cfg_voice_pitch (void)       { return wb_settings_get_voice_pitch (wb_settings_get ()); }
static inline gboolean cfg_use_recorded_audio (void)
{
  return wb_settings_get_use_recorded_audio (wb_settings_get ());
}
static inline int  cfg_window_width (void)            { return wb_settings_get_window_width (wb_settings_get ()); }
static inline int  cfg_window_height (void)           { return wb_settings_get_window_height (wb_settings_get ()); }
static inline char **cfg_history (void)               { return wb_settings_get_history (wb_settings_get ()); }
static inline gboolean cfg_is_favorite (const char *t){ return wb_settings_is_favorite (wb_settings_get (), t); }
static inline void cfg_add_favorite (const char *t)   { wb_settings_add_favorite (wb_settings_get (), t); }
static inline void cfg_remove_favorite (const char *t){ wb_settings_remove_favorite (wb_settings_get (), t); }
static inline void cfg_save_state (const char *const *history, int w, int h)
{
  wb_settings_save_state (wb_settings_get (), history, w, h);
}

static inline char *base_clean_search_terms (const char *s)    { return wb_clean_search_terms (s); }
static inline WbWordnet *base_get_wn_instance (void)           { return wb_get_wn_instance (); }
static inline void  base_wn_free (WbWordnet *wn)               { wb_wordnet_free (wn); }
static inline WbDefinition *base_format_output (const char *t, WbWordnet *wn, const char *accent)
{
  return wb_format_output (t, wn, accent);
}
static inline void  base_definition_free (WbDefinition *d)     { wb_definition_free (d); }
static inline void  base_read_term (const char *lemma, const char *accent, const char *ipa)
{
  wb_read_term (lemma, 150, accent, ipa, cfg_voice_variant (), cfg_voice_pitch ());
}

static inline gboolean database_setup (void) { return wb_database_setup (); }

typedef WbSearchCompletion SearchCompletion;
static inline SearchCompletion *completion_new (GtkWidget *parent, GtkEntry *entry,
                                                char **(*provider) (const char *, int, gpointer),
                                                void (*activate) (const char *, gpointer),
                                                gpointer ud)
{
  return wb_search_completion_new (parent, entry, provider, activate, ud);
}
static inline void completion_free (SearchCompletion *c)                  { wb_search_completion_unref (c); }
static inline void completion_set_enabled (SearchCompletion *c, gboolean e){ wb_search_completion_set_enabled (c, e); }
static inline void completion_update (SearchCompletion *c, const char *t) { wb_search_completion_update (c, t); }
static inline void completion_clear (SearchCompletion *c)                 { wb_search_completion_clear (c); }
static inline gboolean completion_entry_has_focus (SearchCompletion *c)   { return wb_search_completion_entry_has_focus (c); }

static inline void show_preferences_dialog (DictionaryWindow *self)
{
  SettingsDialog *dialog = settings_dialog_new (self);
  adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (self));
}

#define LIVE_SEARCH_DELAY_MS   400
#define HISTORY_DELAY_MS       2000
#define HISTORY_MAX_ITEMS      50
#define MAX_COMPLETIONS        32
#define MAX_SUGGESTIONS        5
#define SUGGEST_SCORE_MIN      70
#define PRIMARY_CLIPBOARD_JUNK "         "

typedef enum {
  SEARCH_STATUS_NONE,
  SEARCH_STATUS_SUCCESS,
  SEARCH_STATUS_FAILURE,
  SEARCH_STATUS_RESET,
} SearchStatus;

typedef enum {
  PAGE_CONTENT,
  PAGE_DB_ERROR,
  PAGE_SEARCH_FAIL,
  PAGE_SPINNER,
  PAGE_WELCOME,
} Page;

static const char *const PAGE_NAMES[] = {
  [PAGE_CONTENT]     = "content_page",
  [PAGE_DB_ERROR]    = "db_error_page",
  [PAGE_SEARCH_FAIL] = "search_fail_page",
  [PAGE_SPINNER]     = "spinner_page",
  [PAGE_WELCOME]     = "welcome_page",
};

static const char *
utf8_lskip (const char *s)
{
  while (*s)
    {
      if ((guchar) *s < 0x80)
        {
          if (!g_ascii_isspace (*s))
            break;
          s++;
        }
      else
        {
          if (!g_unichar_isspace (g_utf8_get_char (s)))
            break;
          s = g_utf8_next_char (s);
        }
    }
  return s;
}

static char *
utf8_strip_dup (const char *s)
{
  const char *start = utf8_lskip (s);
  const char *end = start + strlen (start);

  while (end > start)
    {
      const char *prev = g_utf8_prev_char (end);
      if (!g_unichar_isspace (g_utf8_get_char (prev)))
        break;
      end = prev;
    }
  return g_strndup (start, (gsize) (end - start));
}

static inline gboolean
utf8_is_blank (const char *s)
{
  return *utf8_lskip (s) == '\0';
}

static char *
casefold_dup (const char *s)
{
  gboolean ascii = TRUE;

  for (const guchar *p = (const guchar *) s; *p; p++)
    if (*p >= 0x80)
      {
        ascii = FALSE;
        break;
      }

  if (!ascii)
    return g_utf8_casefold (s, -1);

  char *out = g_strdup (s);
  for (char *p = out; *p; p++)
    if (*p >= 'A' && *p <= 'Z')
      *p = (char) (*p + 32);
  return out;
}

static char *
str_remove_all (const char *s, const char *needle)
{
  const gsize nlen = strlen (needle);
  GString *out = g_string_sized_new (strlen (s));
  const char *hit;

  while ((hit = strstr (s, needle)) != NULL)
    {
      g_string_append_len (out, s, hit - s);
      s = hit + nlen;
    }
  g_string_append (out, s);
  return g_string_free (out, FALSE);
}

#if defined(__GNUC__) || defined(__clang__)
#define popcount64(x) ((guint) __builtin_popcountll (x))
#else
static inline guint
popcount64 (guint64 x)
{
  guint n = 0;
  while (x)
    {
      x &= x - 1;
      n++;
    }
  return n;
}
#endif

typedef struct
{
  const char *fold;
  const char *orig;
  guint64     key;
  guint32     rank;
  guint16     orig_nchars;
} WordEntry;

typedef struct
{
  WordEntry    *entries;
  gsize         n;
  char        **words;
  GStringChunk *folds;
} WordIndex;

static void
word_index_clear (gpointer p)
{
  WordIndex *ix = p;

  g_free (ix->entries);
  if (ix->words)
    {
      for (char **w = ix->words; *w; w++)
        g_free (*w);
      g_free (ix->words);
    }
  if (ix->folds)
    g_string_chunk_free (ix->folds);
}

static inline WordIndex *
word_index_ref (WordIndex *ix)
{
  return ix ? g_atomic_rc_box_acquire (ix) : NULL;
}

static inline void
word_index_unref (WordIndex *ix)
{
  if (ix)
    g_atomic_rc_box_release_full (ix, word_index_clear);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (WordIndex, word_index_unref)

static guint64
prefix_key (const char *s)
{
  guint64 k = 0;
  int i = 0;

  for (; i < 8 && s[i]; i++)
    k = (k << 8) | (guchar) s[i];
  for (; i < 8; i++)
    k <<= 8;
  return k;
}

static void
fold_metrics (const char *f, guint *spaces, guint *nonalnum, guint *nchars)
{
  guint sp = 0, na = 0, n = 0;

  for (const char *p = f; *p;)
    {
      guchar c = (guchar) *p;
      n++;
      if (c < 0x80)
        {
          if (c == ' ')
            sp++;
          if (!g_ascii_isalnum (c))
            na++;
          p++;
        }
      else
        {
          if (!g_unichar_isalnum (g_utf8_get_char (p)))
            na++;
          p = g_utf8_next_char (p);
        }
    }
  *spaces = sp;
  *nonalnum = na;
  *nchars = n;
}

static gint
entry_cmp_fold (gconstpointer a, gconstpointer b, gpointer unused)
{
  const WordEntry *x = a, *y = b;

  if (x->key != y->key)
    return x->key < y->key ? -1 : 1;
  return strcmp (x->fold, y->fold);
}

static WordIndex *
word_index_new_take (GPtrArray *lemmas)
{
  gsize n_in = 0;
  char **in = (char **) g_ptr_array_steal (lemmas, &n_in);

  g_ptr_array_unref (lemmas);

  if (n_in == 0)
    {
      g_free (in);
      return NULL;
    }

  WordIndex *ix = g_atomic_rc_box_new0 (WordIndex);
  ix->entries = g_new (WordEntry, n_in);
  ix->words = g_new0 (char *, n_in + 1);
  ix->folds = g_string_chunk_new (64 * 1024);

  gsize n = 0, n_words = 0;
  for (gsize i = 0; i < n_in; i++)
    {
      char *w = in[i];

      if (!w || !g_utf8_validate (w, -1, NULL))
        {
          g_free (w);
          continue;
        }

      ix->words[n_words++] = w;

      const char *fold = w;
      gboolean ascii = TRUE, has_upper = FALSE;
      for (const guchar *p = (const guchar *) w; *p; p++)
        {
          if (*p >= 0x80)
            {
              ascii = FALSE;
              break;
            }
          if (*p >= 'A' && *p <= 'Z')
            has_upper = TRUE;
        }

      if (ascii && has_upper)
        {
          char *f = g_string_chunk_insert (ix->folds, w);
          for (char *p = f; *p; p++)
            if (*p >= 'A' && *p <= 'Z')
              *p = (char) (*p + 32);
          fold = f;
        }
      else if (!ascii)
        {
          char *f = g_utf8_casefold (w, -1);
          if (strcmp (f, w) != 0)
            fold = g_string_chunk_insert (ix->folds, f);
          g_free (f);
        }

      guint sp, na, nc, onc;
      guint dummy1, dummy2;
      fold_metrics (fold, &sp, &na, &nc);
      if (fold == w)
        onc = nc;
      else
        fold_metrics (w, &dummy1, &dummy2, &onc);

      WordEntry *e = &ix->entries[n++];
      e->fold = fold;
      e->orig = w;
      e->key = prefix_key (fold);
      e->rank = (MIN (sp, 255u) << 24) | (MIN (na, 255u) << 16) | MIN (nc, 65535u);
      e->orig_nchars = (guint16) MIN (onc, 65535u);
    }

  g_free (in);
  ix->n = n;

  if (n == 0)
    {
      word_index_unref (ix);
      return NULL;
    }

  wb_sort_array (ix->entries, (gint) n, sizeof (WordEntry), entry_cmp_fold, NULL);
  return ix;
}

static gsize
word_index_lower_bound (const WordIndex *ix, const char *term, guint64 tkey)
{
  gsize lo = 0, hi = ix->n;

  while (lo < hi)
    {
      gsize mid = lo + (hi - lo) / 2;
      const WordEntry *e = &ix->entries[mid];
      int c;

      if (e->key != tkey)
        c = e->key < tkey ? -1 : 1;
      else
        c = strcmp (e->fold, term);

      if (c < 0)
        lo = mid + 1;
      else
        hi = mid;
    }
  return lo;
}

typedef struct
{
  guint32     rank;
  const char *fold;
  const char *orig;
} Candidate;

static inline gboolean
candidate_less (const Candidate *a, const Candidate *b)
{
  if (a->rank != b->rank)
    return a->rank < b->rank;
  int c = strcmp (a->fold, b->fold);
  if (c != 0)
    return c < 0;
  return strcmp (a->orig, b->orig) < 0;
}

static char **
word_index_complete (const WordIndex *ix, const char *text, int limit)
{
  if (!ix || ix->n == 0 || limit <= 0)
    return NULL;

  const char *typed = utf8_lskip (text);
  if (*typed == '\0')
    return NULL;

  limit = MIN (limit, MAX_COMPLETIONS);

  g_autofree char *term = casefold_dup (typed);
  const gsize tlen = strlen (term);
  const gsize plen = strlen (typed);
  const guint64 tkey = prefix_key (term);

  Candidate best[MAX_COMPLETIONS];
  int nbest = 0;

  gsize i = word_index_lower_bound (ix, term, tkey);
  while (i < ix->n)
    {
      const WordEntry *e = &ix->entries[i];
      if (strncmp (e->fold, term, tlen) != 0)
        break;

      const char *rep = e->orig;
      gboolean rep_ok = strncmp (rep, typed, plen) == 0;
      gsize j = i + 1;
      while (j < ix->n)
        {
          const WordEntry *o = &ix->entries[j];
          if (o->key != e->key || o->rank != e->rank || strcmp (o->fold, e->fold) != 0)
            break;
          if (!rep_ok && strncmp (o->orig, typed, plen) == 0)
            {
              rep = o->orig;
              rep_ok = TRUE;
            }
          j++;
        }

      if (nbest == limit && e->rank > best[nbest - 1].rank)
        {
          i = j;
          continue;
        }

      Candidate c = { e->rank, e->fold, rep };
      int pos = nbest;
      while (pos > 0 && candidate_less (&c, &best[pos - 1]))
        pos--;

      if (nbest < limit)
        {
          memmove (&best[pos + 1], &best[pos], (gsize) (nbest - pos) * sizeof (Candidate));
          best[pos] = c;
          nbest++;
        }
      else if (pos < nbest)
        {
          memmove (&best[pos + 1], &best[pos], (gsize) (nbest - pos - 1) * sizeof (Candidate));
          best[pos] = c;
        }
      i = j;
    }

  if (nbest == 0)
    return NULL;

  char **out = g_new (char *, (gsize) nbest + 1);
  for (int k = 0; k < nbest; k++)
    out[k] = g_strdup (best[k].orig);
  out[nbest] = NULL;
  return out;
}

typedef struct
{
  guint64   ascii[128];
  gunichar *wide;
  guint64  *wide_mask;
  guint     n_wide;
  gunichar *q;
  glong     m;
} Pattern;

static void
pattern_init (Pattern *p, const char *text)
{
  memset (p, 0, sizeof *p);
  p->q = g_utf8_to_ucs4_fast (text, -1, &p->m);

  if (p->m > 64)
    return;

  p->wide = g_new (gunichar, (gsize) p->m);
  p->wide_mask = g_new0 (guint64, (gsize) p->m);

  for (glong i = 0; i < p->m; i++)
    {
      gunichar c = p->q[i];
      guint64 bit = G_GUINT64_CONSTANT (1) << i;

      if (c < 0x80)
        p->ascii[c] |= bit;
      else
        {
          guint k = 0;
          while (k < p->n_wide && p->wide[k] != c)
            k++;
          if (k == p->n_wide)
            p->wide[p->n_wide++] = c;
          p->wide_mask[k] |= bit;
        }
    }
}

static void
pattern_clear (Pattern *p)
{
  g_free (p->q);
  g_free (p->wide);
  g_free (p->wide_mask);
}

static inline guint64
pattern_wide (const Pattern *p, gunichar c)
{
  for (guint k = 0; k < p->n_wide; k++)
    if (p->wide[k] == c)
      return p->wide_mask[k];
  return 0;
}

static inline guint
lcs_bitparallel (const Pattern *p, const char *cand)
{
  guint64 S = ~G_GUINT64_CONSTANT (0);

  for (const char *s = cand; *s;)
    {
      guint64 M;
      guchar c = (guchar) *s;

      if (c < 0x80)
        {
          M = p->ascii[c];
          s++;
        }
      else
        {
          M = pattern_wide (p, g_utf8_get_char (s));
          s = g_utf8_next_char (s);
        }

      guint64 u = S & M;
      S = (S + u) | (S - u);
    }

  guint64 mask = p->m == 64 ? ~G_GUINT64_CONSTANT (0) : ((G_GUINT64_CONSTANT (1) << p->m) - 1);
  return popcount64 (~S & mask);
}

static guint
lcs_dp (const Pattern *p, const char *cand)
{
  glong n = 0;
  g_autofree gunichar *t = g_utf8_to_ucs4_fast (cand, -1, &n);
  g_autofree guint *prev = g_new0 (guint, (gsize) n + 1);
  g_autofree guint *cur = g_new0 (guint, (gsize) n + 1);

  for (glong i = 1; i <= p->m; i++)
    {
      for (glong j = 1; j <= n; j++)
        cur[j] = p->q[i - 1] == t[j - 1] ? prev[j - 1] + 1 : MAX (prev[j], cur[j - 1]);
      guint *tmp = prev;
      prev = cur;
      cur = tmp;
    }
  return prev[n];
}

typedef struct
{
  double score;
  gsize  idx;
} Hit;

static char **
word_index_suggest (const WordIndex *ix, const char *text, GCancellable *cancellable)
{
  if (!ix || ix->n == 0)
    return NULL;

  if (g_utf8_strlen (text, -1) <= 2)
    return NULL;

  Pattern pat;
  pattern_init (&pat, text);

  const guint m = (guint) pat.m;
  Hit hits[MAX_SUGGESTIONS];
  int nhits = 0;

  for (gsize i = 0; i < ix->n; i++)
    {
      if ((i & 0x1FFF) == 0 && cancellable && g_cancellable_is_cancelled (cancellable))
        {
          pattern_clear (&pat);
          return NULL;
        }

      const WordEntry *e = &ix->entries[i];
      const guint n2 = e->orig_nchars;
      if (n2 == 0)
        continue;

      const guint total = m + n2;
      const guint mn = MIN (m, n2);

      if (200u * mn <= (guint) SUGGEST_SCORE_MIN * total)
        continue;
      if (nhits == MAX_SUGGESTIONS && 200.0 * mn / total <= hits[nhits - 1].score)
        continue;

      guint lcs = pat.m <= 64 ? lcs_bitparallel (&pat, e->orig) : lcs_dp (&pat, e->orig);
      if (200u * lcs <= (guint) SUGGEST_SCORE_MIN * total)
        continue;

      double score = 200.0 * lcs / total;
      if (nhits == MAX_SUGGESTIONS && score <= hits[nhits - 1].score)
        continue;

      int pos = nhits;
      while (pos > 0 && hits[pos - 1].score < score)
        pos--;

      int last = nhits < MAX_SUGGESTIONS ? nhits : MAX_SUGGESTIONS - 1;
      memmove (&hits[pos + 1], &hits[pos], (gsize) (last - pos) * sizeof (Hit));
      hits[pos].score = score;
      hits[pos].idx = i;
      if (nhits < MAX_SUGGESTIONS)
        nhits++;
    }

  pattern_clear (&pat);

  if (nhits == 0)
    return NULL;

  char **out = g_new (char *, (gsize) nhits + 1);
  for (int k = 0; k < nhits; k++)
    out[k] = g_strdup (ix->entries[hits[k].idx].orig);
  out[nhits] = NULL;
  return out;
}

#define DICTIONARY_TYPE_HISTORY_ITEM (dictionary_history_item_get_type ())
G_DECLARE_FINAL_TYPE (DictionaryHistoryItem, dictionary_history_item, DICTIONARY, HISTORY_ITEM, GObject)

struct _DictionaryHistoryItem
{
  GObject  parent_instance;
  char    *term;
  gboolean is_favorite;
};

G_DEFINE_FINAL_TYPE (DictionaryHistoryItem, dictionary_history_item, G_TYPE_OBJECT)

typedef DictionaryHistoryItem HistoryItem;

static void
dictionary_history_item_finalize (GObject *obj)
{
  g_free (DICTIONARY_HISTORY_ITEM (obj)->term);
  G_OBJECT_CLASS (dictionary_history_item_parent_class)->finalize (obj);
}

static void
dictionary_history_item_class_init (DictionaryHistoryItemClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = dictionary_history_item_finalize;
}

static void
dictionary_history_item_init (DictionaryHistoryItem *self)
{
}

static HistoryItem *
history_item_new (const char *term, gboolean is_favorite)
{
  HistoryItem *item = g_object_new (DICTIONARY_TYPE_HISTORY_ITEM, NULL);

  item->term = g_strdup (term);
  item->is_favorite = is_favorite;
  return item;
}

typedef struct
{
  const char    *pos;
  WbLemmaGroups *lemma_groups;
} PosRender;

typedef struct
{
  SearchStatus  status;
  char         *searched_term;
  char         *term;
  WbDefinition *def;
  GArray       *pos_render;
  char        **suggestions;
  gboolean      update_history;
  gboolean      show_invalid_dialog;
} SearchResult;

static void
pos_render_clear (gpointer p)
{
  PosRender *pr = p;

  g_clear_pointer (&pr->lemma_groups, wb_lemma_groups_free);
}

static void
search_result_free (SearchResult *r)
{
  if (!r)
    return;
  if (r->pos_render)
    g_array_unref (r->pos_render);
  if (r->def)
    base_definition_free (r->def);
  g_free (r->searched_term);
  g_free (r->term);
  g_strfreev (r->suggestions);
  g_free (r);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (SearchResult, search_result_free)

typedef struct
{
  char      *text;
  char      *accent;
  WbWordnet *wn;
  WordIndex *index;
  gboolean   update_history;
  gboolean   live_search;
} SearchJob;

static void
search_job_free (SearchJob *job)
{
  g_free (job->text);
  g_free (job->accent);
  word_index_unref (job->index);
  g_free (job);
}

struct _DictionaryWindow
{
  AdwApplicationWindow parent_instance;

  GtkButton           *search_button;
  AdwClamp            *title_clamp;
  GtkEntry            *search_entry;
  GtkToggleButton     *split_view_toggle_button;
  GtkMenuButton       *menu_button;
  AdwViewStack        *history_stack;
  GtkListBox          *history_listbox;
  GtkButton           *clear_history_button;
  GtkToggleButton     *favorites_filter_button;
  AdwOverlaySplitView *main_split_view;
  AdwViewStack        *main_stack;
  GtkScrolledWindow   *main_scroll;
  AdwToastOverlay     *toast_overlay;
  GtkListBox          *definitions_listbox;
  GtkLabel            *search_fail_description_label;
  GtkButton           *exit_button;
  GtkButton           *choose_db_button;
  gboolean             db_prompt_shown;
  GtkEventControllerKey *key_ctrlr;
  GtkWidget           *welcome_words_box;

  char     *lookup_term;
  gboolean  auto_paste_requested;

  WbWordnet *wn;
  WordIndex *index;

  char             *searched_term;
  GCancellable     *search_cancellable;
  SearchCompletion *completion;
  guint             live_search_timer;
  gint8             clear_icon_state;

  GListStore         *history_store;
  GtkFilterListModel *history_model;
  GtkCustomFilter    *favorites_filter;
  gboolean            show_favorites_only;
  guint               history_timer;
  char               *pending_history_text;

  GMenu         *def_extra_menu;
  GSimpleAction *search_selected_action;
  gboolean       doubled;
  gboolean       auto_paste_queued;
};

G_DEFINE_FINAL_TYPE (DictionaryWindow, dictionary_window, ADW_TYPE_APPLICATION_WINDOW)

static void start_search (DictionaryWindow *self, const char *text, gboolean update_history);
static void on_entry_changed (GtkEditable *editable, gpointer user_data);

static void
page_switch (DictionaryWindow *self, Page page)
{
  LOG_INFO ("Switching to page: %s", PAGE_NAMES[page]);

  if (page == PAGE_CONTENT)
    gtk_adjustment_set_value (gtk_scrolled_window_get_vadjustment (self->main_scroll), 0);
  adw_view_stack_set_visible_child_name (self->main_stack, PAGE_NAMES[page]);
}

static void
show_error (DictionaryWindow *self, const char *primary, const char *secondary)
{
  AdwDialog *dialog = adw_alert_dialog_new (primary, secondary);

  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "dismiss", _("Dismiss"));
  adw_dialog_present (dialog, GTK_WIDGET (self));
}

static void
set_header_sensitive (DictionaryWindow *self, gboolean sensitive)
{
  gtk_widget_set_sensitive (GTK_WIDGET (self->title_clamp), sensitive);
  gtk_widget_set_sensitive (GTK_WIDGET (self->split_view_toggle_button), sensitive);
  gtk_widget_set_sensitive (GTK_WIDGET (self->menu_button), sensitive);
}

static void
update_clear_button_sensitivity (DictionaryWindow *self)
{
  gboolean has_history = g_list_model_get_n_items (G_LIST_MODEL (self->history_store)) > 0;

  gtk_widget_set_sensitive (GTK_WIDGET (self->clear_history_button), has_history);
  adw_view_stack_set_visible_child_name (self->history_stack, has_history ? "list" : "empty");
}

static void
on_history_items_changed (GListModel *model, guint position, guint removed, guint added, gpointer user_data)
{
  update_clear_button_sensitivity (user_data);
}

static const char *
favorite_icon (gboolean is_favorite)
{
  return is_favorite ? "starred-symbolic" : "non-starred-symbolic";
}

static void
on_favorite_toggled (GtkButton *button, gpointer user_data)
{
  HistoryItem *item = g_object_get_data (G_OBJECT (button), "wb-item");
  DictionaryWindow *self = DICTIONARY_WINDOW (gtk_widget_get_ancestor (GTK_WIDGET (button), DICTIONARY_TYPE_WINDOW));

  if (!item || !self)
    return;

  item->is_favorite = !item->is_favorite;
  if (item->is_favorite)
    cfg_add_favorite (item->term);
  else
    cfg_remove_favorite (item->term);

  GtkWidget *row = gtk_widget_get_ancestor (GTK_WIDGET (button), GTK_TYPE_LIST_BOX_ROW);
  gtk_button_set_icon_name (button, favorite_icon (item->is_favorite));
  if (row)
    {
      if (item->is_favorite)
        gtk_widget_add_css_class (row, "favorite-item");
      else
        gtk_widget_remove_css_class (row, "favorite-item");
    }

  if (self->show_favorites_only)
    gtk_filter_changed (GTK_FILTER (self->favorites_filter),
                        item->is_favorite ? GTK_FILTER_CHANGE_LESS_STRICT : GTK_FILTER_CHANGE_MORE_STRICT);
}

static GtkWidget *
create_history_row (gpointer item_ptr, gpointer user_data)
{
  HistoryItem *item = item_ptr;

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *content_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand (content_box, TRUE);
  gtk_widget_set_tooltip_text (content_box, item->term);

  GtkWidget *label = gtk_label_new (item->term);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_margin_top (label, 8);
  gtk_widget_set_margin_bottom (label, 8);
  gtk_widget_set_margin_start (label, 8);
  gtk_widget_set_margin_end (label, 8);
  gtk_widget_set_halign (label, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (content_box), label);

  GtkWidget *fav = gtk_button_new_from_icon_name (favorite_icon (item->is_favorite));
  gtk_widget_set_margin_top (fav, 4);
  gtk_widget_set_margin_bottom (fav, 4);
  gtk_widget_set_valign (fav, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (fav, _("Toggle Favorite"));
  gtk_widget_add_css_class (fav, "flat");
  gtk_widget_add_css_class (fav, "circular");
  g_object_set_data_full (G_OBJECT (fav), "wb-item", g_object_ref (item), g_object_unref);
  g_signal_connect (fav, "clicked", G_CALLBACK (on_favorite_toggled), NULL);

  gtk_box_append (GTK_BOX (box), content_box);
  gtk_box_append (GTK_BOX (box), fav);

  GtkWidget *row = gtk_list_box_row_new ();
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
  gtk_widget_add_css_class (row, "history-item");
  if (item->is_favorite)
    gtk_widget_add_css_class (row, "favorite-item");
  return row;
}

static gboolean
favorites_filter_func (gpointer item, gpointer user_data)
{
  DictionaryWindow *self = user_data;

  return !self->show_favorites_only || ((HistoryItem *) item)->is_favorite;
}

static void
on_history_row_activated (GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
  DictionaryWindow *self = user_data;
  guint index = (guint) gtk_list_box_row_get_index (row);
  g_autoptr (DictionaryHistoryItem) item = g_list_model_get_item (G_LIST_MODEL (self->history_model), index);

  if (!item)
    return;

  dictionary_window_trigger_search (self, item->term);
  if (adw_overlay_split_view_get_collapsed (self->main_split_view))
    adw_overlay_split_view_set_show_sidebar (self->main_split_view, FALSE);
}

static void
add_to_history (DictionaryWindow *self, const char *text)
{
  GListModel *model = G_LIST_MODEL (self->history_store);
  guint n = g_list_model_get_n_items (model);

  for (guint i = 0; i < n; i++)
    {
      g_autoptr (DictionaryHistoryItem) item = g_list_model_get_item (model, i);
      if (strcmp (item->term, text) == 0)
        {
          g_list_store_remove (self->history_store, i);
          n--;
          break;
        }
    }

  g_autoptr (DictionaryHistoryItem) fresh = history_item_new (text, cfg_is_favorite (text));
  g_list_store_insert (self->history_store, 0, fresh);
  n++;

  if (n > HISTORY_MAX_ITEMS)
    g_list_store_splice (self->history_store, HISTORY_MAX_ITEMS, n - HISTORY_MAX_ITEMS, NULL, 0);
}

static gboolean
execute_delayed_history_add (gpointer user_data)
{
  DictionaryWindow *self = user_data;

  self->history_timer = 0;
  if (self->pending_history_text)
    {
      add_to_history (self, self->pending_history_text);
      g_clear_pointer (&self->pending_history_text, g_free);
    }
  return G_SOURCE_REMOVE;
}

static void
add_to_history_delayed (DictionaryWindow *self, const char *text)
{
  g_clear_handle_id (&self->history_timer, g_source_remove);
  g_free (self->pending_history_text);
  self->pending_history_text = g_strdup (text);
  self->history_timer = g_timeout_add (HISTORY_DELAY_MS, execute_delayed_history_add, self);
}

typedef struct
{
  guint        index;
  HistoryItem *item;
} RemovedEntry;

typedef struct
{
  DictionaryWindow *win;
  GArray         *removed;
} UndoData;

static void
removed_entry_clear (gpointer p)
{
  g_clear_object (&((RemovedEntry *) p)->item);
}

static void
undo_data_free (gpointer data, GClosure *closure)
{
  UndoData *u = data;

  g_array_unref (u->removed);
  g_free (u);
}

static void
on_undo_clear_history (AdwToast *toast, gpointer user_data)
{
  UndoData *u = user_data;

  for (guint i = 0; i < u->removed->len; i++)
    {
      RemovedEntry *e = &g_array_index (u->removed, RemovedEntry, i);
      g_list_store_insert (u->win->history_store, e->index, e->item);
    }
}

static void
on_clear_history (GtkButton *button, gpointer user_data)
{
  DictionaryWindow *self = user_data;
  GListModel *model = G_LIST_MODEL (self->history_store);
  guint n = g_list_model_get_n_items (model);

  GArray *removed = g_array_new (FALSE, FALSE, sizeof (RemovedEntry));
  g_array_set_clear_func (removed, removed_entry_clear);
  g_autoptr (GPtrArray) kept = g_ptr_array_new_with_free_func (g_object_unref);

  for (guint i = 0; i < n; i++)
    {
      HistoryItem *item = g_list_model_get_item (model, i);
      if (item->is_favorite)
        g_ptr_array_add (kept, item);
      else
        {
          RemovedEntry e = { i, item };
          g_array_append_val (removed, e);
        }
    }

  if (removed->len == 0)
    {
      g_array_unref (removed);
      return;
    }

  g_list_store_splice (self->history_store, 0, n, kept->pdata, kept->len);

  AdwToast *toast = adw_toast_new (_("History cleared"));
  adw_toast_set_button_label (toast, _("Undo"));

  UndoData *u = g_new0 (UndoData, 1);
  u->win = self;
  u->removed = removed;
  g_signal_connect_data (toast, "button-clicked", G_CALLBACK (on_undo_clear_history), u, undo_data_free, 0);
  adw_toast_overlay_add_toast (self->toast_overlay, toast);
}

static void
toggle_favorites_filter (DictionaryWindow *self)
{
  self->show_favorites_only = !self->show_favorites_only;
  gtk_toggle_button_set_active (self->favorites_filter_button, self->show_favorites_only);
  gtk_button_set_icon_name (GTK_BUTTON (self->favorites_filter_button), favorite_icon (self->show_favorites_only));
  gtk_filter_changed (GTK_FILTER (self->favorites_filter), GTK_FILTER_CHANGE_DIFFERENT);
}

typedef enum
{
  CLIP_CLEAN,
  CLIP_SELECTED,
} ClipMode;

typedef struct
{
  DictionaryWindow *win;
  ClipMode        mode;
} ClipJob;

static void
on_clipboard_text (GObject *source, GAsyncResult *res, gpointer user_data)
{
  ClipJob *job = user_data;
  g_autoptr (GError) err = NULL;
  g_autofree char *text = gdk_clipboard_read_text_finish (GDK_CLIPBOARD (source), res, &err);

  if (text)
    {
      g_autofree char *out = NULL;

      if (job->mode == CLIP_CLEAN)
        out = base_clean_search_terms (text);
      else
        {
          g_autofree char *no_junk = str_remove_all (text, PRIMARY_CLIPBOARD_JUNK);
          out = str_remove_all (no_junk, "\n");
        }

      if (out && !utf8_is_blank (out))
        dictionary_window_trigger_search (job->win, out);
    }

  g_object_unref (job->win);
  g_free (job);
}

static void
read_clipboard_and_search (DictionaryWindow *self, GdkClipboard *clipboard, ClipMode mode)
{
  ClipJob *job = g_new (ClipJob, 1);

  job->win = g_object_ref (self);
  job->mode = mode;
  gdk_clipboard_read_text_async (clipboard, NULL, on_clipboard_text, job);
}

static void
paste_search (DictionaryWindow *self)
{
  read_clipboard_and_search (self, gtk_widget_get_clipboard (GTK_WIDGET (self)), CLIP_CLEAN);
}

static void
update_search_selected_enabled (DictionaryWindow *self)
{
  if (!self->search_selected_action)
    return;

  GdkClipboard *primary = gtk_widget_get_primary_clipboard (GTK_WIDGET (self));
  GdkContentFormats *formats = gdk_clipboard_get_formats (primary);

  g_simple_action_set_enabled (self->search_selected_action,
                               gdk_content_formats_contain_gtype (formats, G_TYPE_STRING));
}

static void
on_primary_clipboard_changed (GdkClipboard *clipboard, gpointer user_data)
{
  update_search_selected_enabled (user_data);
}

static gboolean
paste_idle_cb (gpointer user_data)
{
  paste_search (user_data);
  return G_SOURCE_REMOVE;
}

static void
action_paste_search (GSimpleAction *a, GVariant *p, gpointer user_data)
{
  paste_search (user_data);
}

static void
action_toggle_sidebar (GSimpleAction *a, GVariant *p, gpointer user_data)
{
  DictionaryWindow *self = user_data;

  gtk_toggle_button_set_active (self->split_view_toggle_button,
                                !gtk_toggle_button_get_active (self->split_view_toggle_button));
}

static void
action_toggle_menu (GSimpleAction *a, GVariant *p, gpointer user_data)
{
  DictionaryWindow *self = user_data;

  gtk_widget_grab_focus (GTK_WIDGET (self->menu_button));
  gtk_menu_button_set_active (self->menu_button, !gtk_menu_button_get_active (self->menu_button));
}

static void
action_preferences (GSimpleAction *a, GVariant *p, gpointer user_data)
{
  show_preferences_dialog (user_data);
}

static void
action_random_word (GSimpleAction *a, GVariant *p, gpointer user_data)
{
  DictionaryWindow *self = user_data;

  if (!self->wn)
    {
      show_error (self, _("Wordlist Loading"),
                  _("The word list is still loading. Please try again in a moment."));
      return;
    }

  g_auto (GStrv) picks = wb_wordnet_random_lemmas (self->wn, 1);
  if (picks[0])
    dictionary_window_trigger_search (self, picks[0]);
}

static void
action_search_selected (GSimpleAction *a, GVariant *p, gpointer user_data)
{
  DictionaryWindow *self = user_data;

  read_clipboard_and_search (self, gtk_widget_get_primary_clipboard (GTK_WIDGET (self)), CLIP_SELECTED);
}

static void
action_toggle_favorites (GSimpleAction *a, GVariant *p, gpointer user_data)
{
  toggle_favorites_filter (user_data);
}

static void
action_quit (GSimpleAction *a, GVariant *p, gpointer user_data)
{
  gtk_window_close (GTK_WINDOW (user_data));
}

static const GActionEntry win_actions[] = {
  { "paste-search",     action_paste_search },
  { "toggle-sidebar",   action_toggle_sidebar },
  { "toggle-menu",      action_toggle_menu },
  { "preferences",      action_preferences },
  { "random-word",      action_random_word },
  { "search-selected",  action_search_selected },
  { "toggle-favorites", action_toggle_favorites },
  { "quit",             action_quit },
};

typedef struct
{
  char *lemma;
  char *ipa;
  char *audio_url;
} SpeakData;

static void
speak_data_free (gpointer data, GClosure *closure)
{
  SpeakData *d = data;

  g_free (d->lemma);
  g_free (d->ipa);
  g_free (d->audio_url);
  g_free (d);
}

typedef struct
{
  char *lemma;
  char *ipa;
  char *accent;
  char *audio_url;
} SpeakJob;

static gpointer
speak_thread (gpointer data)
{
  SpeakJob *j = data;

  /* Prefer the recorded human pronunciation when the person has opted in
     and one is available; fall back to synthesis otherwise (no network,
     download failed, nothing to play it with, ...). */
  gboolean played = FALSE;
  if (j->audio_url != NULL && cfg_use_recorded_audio ())
    played = wb_play_recorded_pronunciation (j->audio_url);
  if (!played)
    base_read_term (j->lemma, j->accent, j->ipa);

  g_free (j->lemma);
  g_free (j->ipa);
  g_free (j->accent);
  g_free (j->audio_url);
  g_free (j);
  return NULL;
}

static void
on_speak_lemma_clicked (GtkButton *button, gpointer user_data)
{
  const SpeakData *d = user_data;
  SpeakJob *j = g_new0 (SpeakJob, 1);

  j->lemma = g_strdup (d->lemma);
  j->ipa = g_strdup (d->ipa);
  j->accent = g_strdup (cfg_accent_code ());
  j->audio_url = g_strdup (d->audio_url);

  GThread *t = g_thread_new ("dictionary-speak", speak_thread, j);
  g_thread_unref (t);
}

static void
on_def_pressed (GtkGestureClick *click, int n_press, double x, double y, gpointer user_data)
{
  DictionaryWindow *self = user_data;

  self->doubled = cfg_double_click () && n_press == 2;
}

static void
on_def_stopped (GtkGestureClick *click, gpointer user_data)
{
  DictionaryWindow *self = user_data;

  if (self->doubled)
    read_clipboard_and_search (self, gtk_widget_get_primary_clipboard (GTK_WIDGET (self)), CLIP_CLEAN);
}

static void
on_word_button_clicked (GtkButton *button, gpointer user_data)
{
  DictionaryWindow *self = user_data;

  gtk_editable_set_text (GTK_EDITABLE (self->search_entry), gtk_button_get_label (button));
  g_clear_handle_id (&self->live_search_timer, g_source_remove);
  start_search (self, NULL, TRUE);
}

static GtkWidget *
make_selectable_label (DictionaryWindow *self, const char *text, const char *css_class)
{
  GtkWidget *label = gtk_label_new (text);

  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0f);
  gtk_label_set_selectable (GTK_LABEL (label), TRUE);
  gtk_label_set_extra_menu (GTK_LABEL (label), G_MENU_MODEL (self->def_extra_menu));
  gtk_widget_add_css_class (label, css_class);

  GtkGesture *click = gtk_gesture_click_new ();
  g_signal_connect (click, "pressed", G_CALLBACK (on_def_pressed), self);
  g_signal_connect (click, "stopped", G_CALLBACK (on_def_stopped), self);
  gtk_widget_add_controller (label, GTK_EVENT_CONTROLLER (click));
  return label;
}

static GtkWidget *
create_relation_widget (DictionaryWindow *self, const char *relation_type, const char *const *words)
{
  if (!words || !*words)
    return NULL;

  GtkWidget *wrap = adw_wrap_box_new ();
  gtk_widget_set_valign (wrap, GTK_ALIGN_START);
  adw_wrap_box_set_line_spacing (ADW_WRAP_BOX (wrap), 4);
  adw_wrap_box_set_child_spacing (ADW_WRAP_BOX (wrap), 6);

  g_autofree char *caption = g_strconcat (relation_type, ":", NULL);
  GtkWidget *type_label = gtk_label_new (caption);
  gtk_label_set_xalign (GTK_LABEL (type_label), 0.0f);
  gtk_widget_set_valign (type_label, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (type_label, "relation-type");
  adw_wrap_box_append (ADW_WRAP_BOX (wrap), type_label);

  for (const char *const *w = words; *w; w++)
    {
      GtkWidget *button = gtk_button_new_with_label (*w);
      gtk_widget_add_css_class (button, "lemma-button");
      g_signal_connect (button, "clicked", G_CALLBACK (on_word_button_clicked), self);
      adw_wrap_box_append (ADW_WRAP_BOX (wrap), button);
    }
  return wrap;
}

/* ---- Welcome page: a handful of random words to greet the user with ---- */

static GtkWidget *
build_welcome_word_card (DictionaryWindow *self, const char *term)
{
  WbDefinition *d = wb_get_definition (term, self->wn, cfg_accent_code ());

  if (!d || !wb_definition_has_result (d))
    {
      if (d)
        base_definition_free (d);
      return NULL;
    }

  const WbSynsetData *synset = NULL;
  const char *pos_name = NULL;

  for (WbPos pos = 0; pos < WB_POS_COUNT && !synset; pos++)
    {
      guint n = 0;
      const WbSynsetData *synsets = wb_definition_get_synsets (d, pos, &n);

      if (synsets && n > 0)
        {
          synset = &synsets[0];
          pos_name = wb_pos_name (pos);
        }
    }

  if (!synset)
    {
      base_definition_free (d);
      return NULL;
    }

  GtkWidget *card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_add_css_class (card, "card");
  gtk_widget_add_css_class (card, "welcome-word-card");
  gtk_widget_set_size_request (card, 200, -1);
  gtk_widget_set_margin_top (card, 14);
  gtk_widget_set_margin_bottom (card, 14);
  gtk_widget_set_margin_start (card, 14);
  gtk_widget_set_margin_end (card, 14);

  GtkWidget *term_button = gtk_button_new_with_label (wb_definition_get_term (d));
  gtk_widget_add_css_class (term_button, "flat");
  gtk_widget_add_css_class (term_button, "title-4");
  gtk_widget_set_halign (term_button, GTK_ALIGN_START);
  g_signal_connect (term_button, "clicked", G_CALLBACK (on_word_button_clicked), self);
  gtk_box_append (GTK_BOX (card), term_button);

  GtkWidget *pos_label = gtk_label_new (pos_name);
  gtk_label_set_xalign (GTK_LABEL (pos_label), 0.0f);
  gtk_widget_add_css_class (pos_label, "pos-header");
  gtk_widget_add_css_class (pos_label, "dimmed");
  gtk_box_append (GTK_BOX (card), pos_label);

  gtk_box_append (GTK_BOX (card), make_selectable_label (self, synset->definition, "definition"));

  if (synset->syn && *synset->syn)
    {
      GtkWidget *rel = create_relation_widget (self, _("Synonyms"), synset->syn);
      if (rel)
        gtk_box_append (GTK_BOX (card), rel);
    }

  base_definition_free (d);
  return card;
}

static void
clear_box_children (GtkWidget *box)
{
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (box)))
    gtk_box_remove (GTK_BOX (box), child);
}

static void
populate_welcome_words (DictionaryWindow *self)
{
  if (!self->welcome_words_box || !self->wn)
    return;

  clear_box_children (self->welcome_words_box);

  int width = gtk_widget_get_width (GTK_WIDGET (self));
  if (width <= 0)
    width = cfg_window_width ();

  /* Show 2 words on a narrower window, 3 once there's room to breathe. */
  const guint target = (guint) (width < 600 ? 2 : 3);

  /* Pick straight from the database by id: no word list needed, so this works
     the instant the database is open, and every pick has a definition.  A few
     spare picks cover the rare entry whose card can't be built. */
  g_auto (GStrv) picks = wb_wordnet_random_lemmas (self->wn, target + 3);
  guint added = 0;

  for (char **w = picks; *w && added < target; w++)
    {
      GtkWidget *card = build_welcome_word_card (self, *w);
      if (card)
        {
          gtk_box_append (GTK_BOX (self->welcome_words_box), card);
          added++;
        }
    }
}

static GtkWidget *
create_definition_row (DictionaryWindow *self, const WbSynsetData *synset, guint number)
{
  GtkWidget *main_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);

  char num[16];
  g_snprintf (num, sizeof num, "%u", number);
  GtkWidget *number_label = gtk_label_new (num);
  gtk_widget_set_valign (number_label, GTK_ALIGN_START);
  gtk_widget_set_margin_top (number_label, 2);
  gtk_widget_add_css_class (number_label, "definition-number");
  gtk_widget_set_size_request (number_label, 20, -1);
  gtk_box_append (GTK_BOX (main_box), number_label);

  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_hexpand (content, TRUE);

  if (synset->tags && *synset->tags)
    {
      g_autofree char *joined = g_strjoinv (" · ", (char **) synset->tags);
      GtkWidget *tags = gtk_label_new (joined);
      gtk_label_set_xalign (GTK_LABEL (tags), 0.0f);
      gtk_label_set_wrap (GTK_LABEL (tags), TRUE);
      gtk_widget_add_css_class (tags, "sense-tags");
      gtk_box_append (GTK_BOX (content), tags);
    }

  if (synset->parent_definition && *synset->parent_definition)
    {
      g_autofree char *parent = g_strdup_printf ("↳ %s", synset->parent_definition);
      gtk_box_append (GTK_BOX (content), make_selectable_label (self, parent, "parent-gloss"));
    }

  gtk_box_append (GTK_BOX (content), make_selectable_label (self, synset->definition, "definition"));

  for (const char *const *ex = synset->examples; ex && *ex; ex++)
    gtk_box_append (GTK_BOX (content), make_selectable_label (self, *ex, "example-text"));

  static const struct
  {
    const char *title;
    gsize       offset;
  } relations[] = {
    { N_("Synonyms"),       G_STRUCT_OFFSET (WbSynsetData, syn) },
    { N_("Antonyms"),       G_STRUCT_OFFSET (WbSynsetData, ant) },
    { N_("Broader terms"),  G_STRUCT_OFFSET (WbSynsetData, broader) },
    { N_("Narrower terms"), G_STRUCT_OFFSET (WbSynsetData, narrower) },
    { N_("Same category"),  G_STRUCT_OFFSET (WbSynsetData, coordinate) },
    { N_("Word family"),    G_STRUCT_OFFSET (WbSynsetData, derived) },
    { N_("Related"),        G_STRUCT_OFFSET (WbSynsetData, related) },
    { N_("Pertains to"),    G_STRUCT_OFFSET (WbSynsetData, pertains) },
    { N_("Participle"),     G_STRUCT_OFFSET (WbSynsetData, participle) },
  };

  for (guint i = 0; i < G_N_ELEMENTS (relations); i++)
    {
      const char *const *words = G_STRUCT_MEMBER (const char *const *, synset, relations[i].offset);
      GtkWidget *rel = create_relation_widget (self, _(relations[i].title), words);
      if (rel)
        gtk_box_append (GTK_BOX (content), rel);
    }

  gtk_box_append (GTK_BOX (main_box), content);
  return main_box;
}

static void
on_open_uri_clicked (GtkButton *button, gpointer user_data)
{
  const char *uri = g_object_get_data (G_OBJECT (button), "uri");
  if (uri && *uri)
    g_app_info_launch_default_for_uri (uri, NULL, NULL);
}

static GtkWidget *
make_uri_button (const char *label, const char *icon, const char *tooltip, const char *uri)
{
  GtkWidget *b = label ? gtk_button_new_with_label (label) : gtk_button_new_from_icon_name (icon);
  gtk_widget_set_valign (b, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (b, "flat");
  if (tooltip)
    gtk_widget_set_tooltip_text (b, tooltip);
  g_object_set_data_full (G_OBJECT (b), "uri", g_strdup (uri), g_free);
  g_signal_connect (b, "clicked", G_CALLBACK (on_open_uri_clicked), NULL);
  return b;
}

static void
append_pronunciation_controls (GtkBox *box, const WbPronunciation *pron, const char *lemma)
{
  if (!pron)
    return;

  gboolean has_ipa = pron->ipa && *pron->ipa;
  gboolean has_audio = pron->audio_url && *pron->audio_url;
  if (!has_ipa && !has_audio)
    return;

  if (has_ipa)
    {
      GtkWidget *ipa_label = gtk_label_new (pron->ipa);
      gtk_label_set_selectable (GTK_LABEL (ipa_label), TRUE);
      gtk_widget_set_valign (ipa_label, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class (ipa_label, "dimmed");
      gtk_box_append (box, ipa_label);
    }

  GtkWidget *controls = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_widget_set_valign (controls, GTK_ALIGN_CENTER);

  GtkWidget *play = gtk_button_new_from_icon_name ("audio-volume-high-symbolic");
  gtk_widget_set_valign (play, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (play, _("Listen to Pronunciation"));
  gtk_widget_add_css_class (play, "flat");
  gtk_widget_add_css_class (play, "circular");

  SpeakData *d = g_new0 (SpeakData, 1);
  d->lemma = g_strdup (lemma);
  d->ipa = (has_ipa && !pron->is_fallback) ? g_strdup (pron->ipa) : NULL;
  d->audio_url = has_audio ? g_strdup (pron->audio_url) : NULL;
  g_signal_connect_data (play, "clicked", G_CALLBACK (on_speak_lemma_clicked), d, speak_data_free, 0);
  gtk_box_append (GTK_BOX (controls), play);

  if (has_audio)
    {
      g_autofree char *tip = pron->audio_credit
        ? g_strdup_printf (_("Recorded pronunciation — %s%s%s"), pron->audio_credit,
                           pron->audio_license ? ", " : "", pron->audio_license ? pron->audio_license : "")
        : g_strdup (_("Recorded pronunciation"));
      gtk_box_append (GTK_BOX (controls),
                      make_uri_button (NULL, "media-playback-start-symbolic", tip, pron->audio_url));
    }
  gtk_box_append (box, controls);
}

static GtkWidget *
create_header_row (const char *lemma, const WbPronunciation *pron, const char *cefr)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);

  GtkWidget *header = gtk_label_new (lemma);
  gtk_label_set_xalign (GTK_LABEL (header), 0.0f);
  gtk_widget_set_hexpand (header, TRUE);
  gtk_widget_add_css_class (header, "synset-header");
  gtk_label_set_selectable (GTK_LABEL (header), TRUE);
  gtk_box_append (GTK_BOX (box), header);

  append_pronunciation_controls (GTK_BOX (box), pron, lemma);

  if (cefr && *cefr)
    {
      GtkWidget *badge = gtk_label_new (cefr);
      gtk_widget_set_valign (badge, GTK_ALIGN_CENTER);
      gtk_widget_set_margin_start (badge, 6);
      gtk_widget_add_css_class (badge, "cefr-badge");
      gtk_widget_set_tooltip_text (badge, _("CEFR level"));
      gtk_box_append (GTK_BOX (box), badge);
    }
  return box;
}

static GtkWidget *
create_definition_separator (void)
{
  GtkWidget *sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);

  gtk_widget_set_margin_top (sep, 8);
  gtk_widget_set_margin_bottom (sep, 8);
  gtk_widget_set_margin_start (sep, 32);
  return sep;
}

static GtkWidget *
make_chip (const char *text, const char *tooltip)
{
  GtkWidget *l = gtk_label_new (text);
  gtk_widget_add_css_class (l, "meta-chip");
  if (tooltip)
    gtk_widget_set_tooltip_text (l, tooltip);
  return l;
}

/* Entry-level facts: syllables, countability, transitivity, frequency, Wikipedia. */
static GtkWidget *
create_entry_meta_row (const WbSynsetData *m)
{
  gboolean show_freq = (m->freq_bucket && *m->freq_bucket) || m->zipf >= 0;
  if (!(m->hyphenation && *m->hyphenation) && !(m->countable && *m->countable) &&
      !m->transitive && !show_freq && !(m->wiki_url && *m->wiki_url))
    return NULL;

  GtkWidget *wrap = adw_wrap_box_new ();
  adw_wrap_box_set_line_spacing (ADW_WRAP_BOX (wrap), 4);
  adw_wrap_box_set_child_spacing (ADW_WRAP_BOX (wrap), 6);

  if (m->hyphenation && *m->hyphenation)
    adw_wrap_box_append (ADW_WRAP_BOX (wrap), make_chip (m->hyphenation, _("Syllables")));

  if (m->countable && *m->countable)
    {
      const char *txt = g_strcmp0 (m->countable, "both") == 0 ? _("countable & uncountable")
                      : g_strcmp0 (m->countable, "uncountable") == 0 ? _("uncountable")
                      : _("countable");
      adw_wrap_box_append (ADW_WRAP_BOX (wrap), make_chip (txt, NULL));
    }

  if (m->transitive)
    adw_wrap_box_append (ADW_WRAP_BOX (wrap), make_chip (_("transitive"), NULL));

  if (show_freq)
    {
      g_autofree char *tip = m->zipf >= 0 ? g_strdup_printf (_("Zipf frequency %.2f (1–8 scale)"), m->zipf) : NULL;
      g_autofree char *fallback = m->zipf >= 0 ? g_strdup_printf (_("frequency %.1f"), m->zipf) : NULL;
      adw_wrap_box_append (ADW_WRAP_BOX (wrap),
                           make_chip ((m->freq_bucket && *m->freq_bucket) ? m->freq_bucket : fallback, tip));
    }

  if (m->wiki_url && *m->wiki_url)
    adw_wrap_box_append (ADW_WRAP_BOX (wrap),
                         make_uri_button (_("Wikipedia"), NULL, m->wiki_url, m->wiki_url));
  return wrap;
}

static GtkWidget *
create_expander_widget (const char *title, GtkWidget *child)
{
  GtkWidget *expander = gtk_expander_new (title);
  gtk_widget_add_css_class (expander, "entry-expander");
  gtk_expander_set_child (GTK_EXPANDER (expander), child);
  return expander;
}

static GtkWidget *
create_expander (DictionaryWindow *self, const char *title, const char *text, const char *css)
{
  return create_expander_widget (title, make_selectable_label (self, text, css));
}

/* Word family: other inflected forms, each a clickable button that re-searches
   it, with its grammatical tags shown as a small trailing label. */
static GtkWidget *
create_word_family_widget (DictionaryWindow *self, const WbInflectedForm *forms, guint n)
{
  if (n == 0)
    return NULL;

  GtkWidget *wrap = adw_wrap_box_new ();
  gtk_widget_set_valign (wrap, GTK_ALIGN_START);
  adw_wrap_box_set_line_spacing (ADW_WRAP_BOX (wrap), 4);
  adw_wrap_box_set_child_spacing (ADW_WRAP_BOX (wrap), 6);

  for (guint i = 0; i < n; i++)
    {
      GtkWidget *item = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
      gtk_widget_set_valign (item, GTK_ALIGN_CENTER);

      GtkWidget *button = gtk_button_new_with_label (forms[i].form);
      gtk_widget_add_css_class (button, "lemma-button");
      g_signal_connect (button, "clicked", G_CALLBACK (on_word_button_clicked), self);
      gtk_box_append (GTK_BOX (item), button);

      if (forms[i].tags && *forms[i].tags)
        {
          GString *tagtext = g_string_new (NULL);
          for (const char *const *t = forms[i].tags; *t; t++)
            {
              if (tagtext->len) g_string_append (tagtext, ", ");
              g_string_append (tagtext, *t);
            }
          gtk_box_append (GTK_BOX (item), make_chip (tagtext->str, NULL));
          g_string_free (tagtext, TRUE);
        }
      adw_wrap_box_append (ADW_WRAP_BOX (wrap), item);
    }
  return wrap;
}

/* Collocations: inert chips, count shown as a tooltip. */
static GtkWidget *
create_collocations_widget (const WbCollocation *cols, guint n)
{
  if (n == 0)
    return NULL;

  GtkWidget *wrap = adw_wrap_box_new ();
  gtk_widget_set_valign (wrap, GTK_ALIGN_START);
  adw_wrap_box_set_line_spacing (ADW_WRAP_BOX (wrap), 4);
  adw_wrap_box_set_child_spacing (ADW_WRAP_BOX (wrap), 6);

  for (guint i = 0; i < n; i++)
    {
      g_autofree char *tip = g_strdup_printf (_("Seen together %d times"), cols[i].count);
      adw_wrap_box_append (ADW_WRAP_BOX (wrap), make_chip (cols[i].pattern, tip));
    }
  return wrap;
}

/* Extra curated example sentences from the corpus. */
static GtkWidget *
create_extra_examples_widget (DictionaryWindow *self, const char *const *examples)
{
  if (!examples || !*examples)
    return NULL;

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  for (const char *const *ex = examples; *ex; ex++)
    gtk_box_append (GTK_BOX (box), make_selectable_label (self, *ex, "example-text"));
  return box;
}

static GtkWidget *
create_pos_widget (DictionaryWindow *self, const PosRender *pr)
{
  GtkWidget *pos_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top (pos_box, 12);
  gtk_widget_set_margin_bottom (pos_box, 12);
  gtk_widget_set_margin_start (pos_box, 12);
  gtk_widget_set_margin_end (pos_box, 12);

  GtkWidget *pos_label = gtk_label_new (pr->pos);
  gtk_label_set_xalign (GTK_LABEL (pos_label), 0.0f);
  gtk_widget_add_css_class (pos_label, "pos-header");
  gtk_label_set_selectable (GTK_LABEL (pos_label), TRUE);
  gtk_box_append (GTK_BOX (pos_box), pos_label);

  for (guint i = 0; i < pr->lemma_groups->n_groups; i++)
    {
      const WbLemmaGroup *lg = &pr->lemma_groups->groups[i];
      GtkWidget *lemma_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);

      for (guint j = 0; j < lg->n_pronunciation_groups; j++)
        {
          const WbPronunciationGroup *pg = &lg->pronunciation_groups[j];
          GtkWidget *pron_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);

          const WbSynsetData *meta = pg->synsets[0];

          gtk_box_append (GTK_BOX (pron_box), create_header_row (lg->lemma, pg->pronunciation, meta->cefr));

          GtkWidget *meta_row = create_entry_meta_row (meta);
          if (meta_row)
            gtk_box_append (GTK_BOX (pron_box), meta_row);
          if (meta->usage_notes && *meta->usage_notes)
            gtk_box_append (GTK_BOX (pron_box), create_expander (self, _("Usage notes"), meta->usage_notes, "usage-notes"));
          if (meta->etymology && *meta->etymology)
            gtk_box_append (GTK_BOX (pron_box), create_expander (self, _("Etymology"), meta->etymology, "etymology-text"));
          if (meta->n_word_family > 0)
            {
              GtkWidget *wf = create_word_family_widget (self, meta->word_family, meta->n_word_family);
              if (wf) gtk_box_append (GTK_BOX (pron_box), create_expander_widget (_("Word Family"), wf));
            }
          if (meta->n_collocations > 0)
            {
              GtkWidget *co = create_collocations_widget (meta->collocations, meta->n_collocations);
              if (co) gtk_box_append (GTK_BOX (pron_box), create_expander_widget (_("Common Phrases"), co));
            }
          if (meta->extra_examples && *meta->extra_examples)
            {
              GtkWidget *ex = create_extra_examples_widget (self, meta->extra_examples);
              if (ex) gtk_box_append (GTK_BOX (pron_box), create_expander_widget (_("More Examples"), ex));
            }

          for (guint k = 0; k < pg->n_synsets; k++)
            {
              gtk_box_append (GTK_BOX (pron_box),
                              create_definition_row (self, pg->synsets[k], k + 1));
              if (k + 1 < pg->n_synsets)
                gtk_box_append (GTK_BOX (pron_box), create_definition_separator ());
            }
          gtk_box_append (GTK_BOX (lemma_box), pron_box);
        }
      gtk_box_append (GTK_BOX (pos_box), lemma_box);
    }
  return pos_box;
}

static void
clear_definitions (DictionaryWindow *self)
{
  gtk_list_box_remove_all (self->definitions_listbox);
}

static void
populate_definitions (DictionaryWindow *self, const SearchResult *r)
{
  clear_definitions (self);

  for (guint i = 0; i < r->pos_render->len; i++)
    {
      const PosRender *pr = &g_array_index (r->pos_render, PosRender, i);
      if (!pr->lemma_groups || pr->lemma_groups->n_groups == 0)
        continue;

      GtkWidget *row = gtk_list_box_row_new ();
      gtk_widget_set_focusable (row, FALSE);
      gtk_widget_set_margin_top (row, 4);
      gtk_widget_set_margin_bottom (row, 4);
      gtk_widget_set_margin_start (row, 4);
      gtk_widget_set_margin_end (row, 4);
      gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), create_pos_widget (self, pr));
      gtk_list_box_append (self->definitions_listbox, row);

      gtk_widget_remove_css_class (row, "activatable");
    }
}

static SearchResult *
run_search (SearchJob *job, GCancellable *cancellable)
{
  SearchResult *r = g_new0 (SearchResult, 1);

  r->searched_term = g_strdup (job->text);
  r->update_history = job->update_history;
  r->status = SEARCH_STATUS_RESET;

  g_autofree char *clean = base_clean_search_terms (job->text);

  if (!clean || utf8_is_blank (clean))
    {
      r->show_invalid_dialog = !job->live_search;
      return r;
    }
  if (!job->wn)
    return r;

  WbDefinition *def = base_format_output (clean, job->wn, job->accent);
  if (!def)
    return r;

  r->def = def;
  r->term = g_strdup (wb_definition_get_term (def));

  if (wb_definition_has_result (def))
    {
      r->status = SEARCH_STATUS_SUCCESS;
      r->pos_render = g_array_new (FALSE, TRUE, sizeof (PosRender));
      g_array_set_clear_func (r->pos_render, pos_render_clear);

      for (int p = 0; p < WB_POS_COUNT; p++)
        {
          guint n = 0;
          const WbSynsetData *syn = wb_definition_get_synsets (def, (WbPos) p, &n);
          if (!syn || n == 0)
            continue;

          PosRender pr = { wb_pos_name ((WbPos) p), wb_group_synsets_by_lemma (syn, n) };
          g_array_append_val (r->pos_render, pr);
        }
    }
  else
    {
      r->status = SEARCH_STATUS_FAILURE;
      if (g_utf8_strlen (job->text, -1) > 2)
        r->suggestions = word_index_suggest (job->index, job->text, cancellable);
    }

  return r;
}

static void
search_thread_func (GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
  SearchJob *job = task_data;

  if (g_task_return_error_if_cancelled (task))
    return;

  SearchResult *r = run_search (job, cancellable);

  if (g_task_return_error_if_cancelled (task))
    {
      search_result_free (r);
      return;
    }
  g_task_return_pointer (task, r, (GDestroyNotify) search_result_free);
}

static void
finish_search (DictionaryWindow *self, const SearchResult *r)
{
  g_free (self->searched_term);
  self->searched_term = g_strdup (r->searched_term);

  if (r->show_invalid_dialog)
    show_error (self, _("Invalid input"), _("Nothing definable was found in your search input"));

  switch (r->status)
    {
    case SEARCH_STATUS_SUCCESS:
      populate_definitions (self, r);
      page_switch (self, PAGE_CONTENT);

      if (r->update_history)
        {
          if (cfg_live_search ())
            add_to_history_delayed (self, r->term);
          else
            add_to_history (self, r->term);
        }
      break;

    case SEARCH_STATUS_FAILURE:
      if (r->suggestions && *r->suggestions)
        {
          GString *markup = g_string_new ("Did you mean: ");

          for (char **s = r->suggestions; *s; s++)
            {
              g_autofree char *link = g_markup_printf_escaped ("<a href=\"search;%s\">%s</a>", *s, *s);
              if (s != r->suggestions)
                g_string_append (markup, ", ");
              g_string_append (markup, link);
            }
          g_string_append_c (markup, '?');

          gtk_label_set_markup (self->search_fail_description_label, markup->str);
          gtk_widget_set_visible (GTK_WIDGET (self->search_fail_description_label), TRUE);
          g_string_free (markup, TRUE);
        }
      else
        {
          gtk_label_set_markup (self->search_fail_description_label, "");
          gtk_widget_set_visible (GTK_WIDGET (self->search_fail_description_label), FALSE);
        }
      page_switch (self, PAGE_SEARCH_FAIL);
      break;

    case SEARCH_STATUS_NONE:
    case SEARCH_STATUS_RESET:
    default:
      page_switch (self, PAGE_WELCOME);
      break;
    }
}

static void
on_search_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  DictionaryWindow *self = DICTIONARY_WINDOW (source);
  g_autoptr (GError) err = NULL;
  g_autoptr (SearchResult) r = g_task_propagate_pointer (G_TASK (res), &err);

  if (!r || err || g_cancellable_is_cancelled (g_task_get_cancellable (G_TASK (res))))
    return;

  finish_search (self, r);
}

static void
start_search (DictionaryWindow *self, const char *text_in, gboolean update_history)
{
  g_clear_handle_id (&self->live_search_timer, g_source_remove);

  if (self->completion)
    completion_clear (self->completion);

  clear_definitions (self);

  g_autofree char *owned = NULL;
  const char *text = text_in;

  if (!text)
    {
      owned = utf8_strip_dup (gtk_editable_get_text (GTK_EDITABLE (self->search_entry)));
      text = owned;
    }

  if (*text == '\0')
    {
      populate_welcome_words (self);
      page_switch (self, PAGE_WELCOME);
      return;
    }

  if (self->search_cancellable)
    {
      g_cancellable_cancel (self->search_cancellable);
      g_clear_object (&self->search_cancellable);
    }

  page_switch (self, PAGE_SPINNER);

  SearchJob *job = g_new0 (SearchJob, 1);
  job->text = g_strdup (text);
  job->accent = g_strdup (cfg_accent_code ());
  job->wn = self->wn;
  job->index = word_index_ref (self->index);
  job->update_history = update_history;
  job->live_search = cfg_live_search ();

  self->search_cancellable = g_cancellable_new ();
  GTask *task = g_task_new (self, self->search_cancellable, on_search_done, NULL);
  g_task_set_task_data (task, job, (GDestroyNotify) search_job_free);
  g_task_run_in_thread (task, search_thread_func);
  g_object_unref (task);
}

static char **
completion_provider (const char *text, int limit, gpointer user_data)
{
  DictionaryWindow *self = user_data;

  return word_index_complete (self->index, text, limit);
}

static void
completion_activate (const char *text, gpointer user_data)
{
  dictionary_window_trigger_search (user_data, text);
}

static gboolean
execute_delayed_search (gpointer user_data)
{
  DictionaryWindow *self = user_data;

  self->live_search_timer = 0;
  start_search (self, NULL, TRUE);
  return G_SOURCE_REMOVE;
}

static void
on_entry_changed (GtkEditable *editable, gpointer user_data)
{
  DictionaryWindow *self = user_data;
  const char *text = gtk_editable_get_text (editable);
  gint8 has_text = text[0] != '\0';

  if (self->clear_icon_state != has_text)
    {
      gtk_entry_set_icon_from_icon_name (self->search_entry, GTK_ENTRY_ICON_SECONDARY,
                                         has_text ? "edit-clear-symbolic" : NULL);
      self->clear_icon_state = has_text;
    }

  if (self->completion)
    completion_update (self->completion, text);

  if (cfg_live_search ())
    {
      g_clear_handle_id (&self->live_search_timer, g_source_remove);
      self->live_search_timer = g_timeout_add (LIVE_SEARCH_DELAY_MS, execute_delayed_search, self);
    }
}

static void
on_entry_icon_pressed (GtkEntry *entry, GtkEntryIconPosition pos, gpointer user_data)
{
  if (pos == GTK_ENTRY_ICON_SECONDARY)
    dictionary_window_trigger_search (user_data, "");
}

static gboolean
on_key_pressed (GtkEventControllerKey *ctrl, guint keyval, guint keycode, GdkModifierType state, gpointer user_data)
{
  DictionaryWindow *self = user_data;
  GdkModifierType mods = state & gtk_accelerator_get_default_mod_mask ();
  gunichar ch = gdk_keyval_to_unicode (keyval);

  if (ch != 0 && g_unichar_isgraph (ch) && (mods == 0 || mods == GDK_SHIFT_MASK) &&
      !(self->completion && completion_entry_has_focus (self->completion)))
    {
      char buf[8];
      int len = g_unichar_to_utf8 (ch, buf);
      int pos = -1;

      gtk_entry_grab_focus_without_selecting (self->search_entry);
      gtk_editable_insert_text (GTK_EDITABLE (self->search_entry), buf, len, &pos);
      gtk_editable_set_position (GTK_EDITABLE (self->search_entry), -1);
      return GDK_EVENT_STOP;
    }
  return GDK_EVENT_PROPAGATE;
}

static void
on_search_clicked (GtkButton *button, gpointer user_data)
{
  start_search (user_data, NULL, TRUE);
}

static gboolean
on_link_activated (GtkLabel *label, const char *uri, gpointer user_data)
{
  if (g_str_has_prefix (uri, "search;"))
    dictionary_window_trigger_search (user_data, uri + strlen ("search;"));
  return GDK_EVENT_STOP;
}

static void
on_exit_clicked (GtkButton *button, gpointer user_data)
{
  GApplication *app = g_application_get_default ();

  if (app)
    g_application_quit (app);
  else
    exit (0);
}

static void
on_is_active_changed (GObject *obj, GParamSpec *pspec, gpointer user_data)
{
  DictionaryWindow *self = user_data;

  if (self->auto_paste_queued && gtk_window_is_active (GTK_WINDOW (self)))
    {
      self->auto_paste_queued = FALSE;
      g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, paste_idle_cb, g_object_ref (self), g_object_unref);
    }
}

typedef struct
{
  DictionaryWindow *win;
  WordIndex      *index;
} IndexDelivery;

static gboolean
deliver_index_cb (gpointer user_data)
{
  IndexDelivery *d = user_data;
  DictionaryWindow *self = d->win;

  word_index_unref (self->index);
  self->index = d->index;
  d->index = NULL;

  LOG_INFO ("Wordlist loaded with %lu words. Completions now available.",
            (unsigned long) (self->index ? self->index->n : 0));

  wb_release_memory ();
  return G_SOURCE_REMOVE;
}

static void
delivery_free (gpointer data)
{
  IndexDelivery *d = data;

  word_index_unref (d->index);
  g_object_unref (d->win);
  g_free (d);
}

static void
on_wordlist_ready (WbWordlist *list, gpointer user_data)
{
  IndexDelivery *d = g_new0 (IndexDelivery, 1);
  GPtrArray *lemmas = g_ptr_array_new ();
  guint n = wb_wordlist_size (list);

  for (guint i = 0; i < n; i++)
    g_ptr_array_add (lemmas, g_strdup (wb_wordlist_get (list, i)));
  wb_wordlist_free (list);

  d->win = user_data;
  d->index = word_index_new_take (lemmas);
  g_idle_add_full (G_PRIORITY_DEFAULT, deliver_index_cb, d, delivery_free);
}

static void
complete_initialization (DictionaryWindow *self)
{
  set_header_sensitive (self, TRUE);
  page_switch (self, PAGE_WELCOME);

  if (self->lookup_term && *self->lookup_term)
    dictionary_window_trigger_search (self, self->lookup_term);
  else if (cfg_auto_paste_on_launch () || self->auto_paste_requested)
    dictionary_window_queue_auto_paste (self);

  gtk_entry_grab_focus_without_selecting (self->search_entry);
}

static void setup_wn (DictionaryWindow *self);

/* The person picked a database (from the first-run prompt or the error
 * page): try again right away - no restart needed. */
static void
on_db_picked (gboolean picked, gpointer user_data)
{
  if (picked)
    setup_wn (DICTIONARY_WINDOW (user_data));
}

static void
on_choose_db_clicked (GtkButton *button, DictionaryWindow *self)
{
  settings_pick_database_async (GTK_WIDGET (self), on_db_picked, self);
}

static void
on_db_prompt_response (AdwAlertDialog *dialog, const char *response, DictionaryWindow *self)
{
  if (g_strcmp0 (response, "choose") == 0)
    settings_pick_database_async (GTK_WIDGET (self), on_db_picked, self);
}

/* No database anywhere we looked: ask where it is, once per window. The
 * error page keeps a "Choose Database…" button for later. */
static void
prompt_for_database (DictionaryWindow *self)
{
  if (self->db_prompt_shown)
    return;
  self->db_prompt_shown = TRUE;

  g_autofree char *name = g_strdup_printf (WB_DB_FILE_FMT, WN_FILE_VERSION);
  g_autofree char *body = g_strdup_printf (
    _("Dictionary couldn't find its database (%s). Choose where it is stored on your computer."),
    name);

  AdwDialog *alert = adw_alert_dialog_new (_("Database Not Found"), body);
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (alert),
                                  "later",  _("_Not Now"),
                                  "choose", _("_Choose File…"),
                                  NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (alert), "choose",
                                            ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (alert), "choose");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (alert), "later");
  g_signal_connect (alert, "response", G_CALLBACK (on_db_prompt_response), self);
  adw_dialog_present (alert, GTK_WIDGET (self));
}

static void
setup_thread_func (GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
  if (!database_setup ())
    {
      g_task_return_pointer (task, NULL, NULL);
      return;
    }
  g_task_return_pointer (task, base_get_wn_instance (), NULL);
}

static void
on_setup_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  DictionaryWindow *self = DICTIONARY_WINDOW (source);
  WbWordnet *wn = g_task_propagate_pointer (G_TASK (res), NULL);

  if (!wn)
    {
      LOG_ERROR ("Database setup failed");
      page_switch (self, PAGE_DB_ERROR);
      prompt_for_database (self);
      return;
    }

  self->wn = wn;
  complete_initialization (self);
  populate_welcome_words (self);
  wb_wordlist_load_async (wn, on_wordlist_ready, g_object_ref (self));
}

static void
setup_wn (DictionaryWindow *self)
{
  set_header_sensitive (self, FALSE);
  page_switch (self, PAGE_SPINNER);

  GTask *task = g_task_new (self, NULL, on_setup_done, NULL);
  g_task_run_in_thread (task, setup_thread_func);
  g_object_unref (task);
}

static void
setup_widgets (DictionaryWindow *self)
{
  self->history_store = g_list_store_new (DICTIONARY_TYPE_HISTORY_ITEM);
  GtkCustomFilter *filter = gtk_custom_filter_new (favorites_filter_func, self, NULL);
  self->favorites_filter = filter;
  self->history_model = gtk_filter_list_model_new (G_LIST_MODEL (g_object_ref (self->history_store)), GTK_FILTER (filter));
  gtk_list_box_bind_model (self->history_listbox, G_LIST_MODEL (self->history_model),
                           create_history_row, self, NULL);
  g_signal_connect (self->history_listbox, "row-activated", G_CALLBACK (on_history_row_activated), self);
  g_signal_connect (self->history_store, "items-changed", G_CALLBACK (on_history_items_changed), self);

  g_signal_connect (self, "notify::is-active", G_CALLBACK (on_is_active_changed), self);
  g_signal_connect (self->key_ctrlr, "key-pressed", G_CALLBACK (on_key_pressed), self);
  g_signal_connect (self->search_fail_description_label, "activate-link", G_CALLBACK (on_link_activated), self);
  g_signal_connect (self->search_button, "clicked", G_CALLBACK (on_search_clicked), self);
  g_signal_connect (self->search_entry, "changed", G_CALLBACK (on_entry_changed), self);
  g_signal_connect (self->search_entry, "icon-press", G_CALLBACK (on_entry_icon_pressed), self);
  g_signal_connect (self->exit_button, "clicked", G_CALLBACK (on_exit_clicked), self);
  g_signal_connect (self->choose_db_button, "clicked", G_CALLBACK (on_choose_db_clicked), self);
  g_signal_connect (self->clear_history_button, "clicked", G_CALLBACK (on_clear_history), self);

  g_object_bind_property (self->main_split_view, "show-sidebar",
                          self->split_view_toggle_button, "active",
                          G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);

  self->def_extra_menu = g_menu_new ();
  g_menu_append (self->def_extra_menu, _("Search Selected Text"), "win.search-selected");

  self->completion = completion_new (GTK_WIDGET (self), self->search_entry,
                                     completion_provider, completion_activate, self);
  completion_set_enabled (self->completion, !cfg_live_search ());

  g_auto (GStrv) history = cfg_history ();
  g_autoptr (GPtrArray) items = g_ptr_array_new_with_free_func (g_object_unref);
  for (char **t = history; t && *t; t++)
    g_ptr_array_add (items, history_item_new (*t, cfg_is_favorite (*t)));
  g_list_store_splice (self->history_store, 0, 0, items->pdata, items->len);
  update_clear_button_sensitivity (self);

  gtk_widget_set_visible (GTK_WIDGET (self->search_button), !cfg_live_search ());
  if (!cfg_live_search ())
    gtk_window_set_default_widget (GTK_WINDOW (self), GTK_WIDGET (self->search_button));

  setup_wn (self);
}

static void
setup_actions (DictionaryWindow *self)
{
  g_action_map_add_action_entries (G_ACTION_MAP (self), win_actions, G_N_ELEMENTS (win_actions), self);

  self->search_selected_action =
      G_SIMPLE_ACTION (g_action_map_lookup_action (G_ACTION_MAP (self), "search-selected"));
  g_simple_action_set_enabled (self->search_selected_action, FALSE);

  GdkClipboard *primary = gtk_widget_get_primary_clipboard (GTK_WIDGET (self));
  g_signal_connect_object (primary, "changed", G_CALLBACK (on_primary_clipboard_changed), self, 0);
  update_search_selected_enabled (self);
}

static void
dictionary_window_dispose (GObject *obj)
{
  DictionaryWindow *self = DICTIONARY_WINDOW (obj);

  if (self->search_cancellable)
    {
      g_cancellable_cancel (self->search_cancellable);
      g_clear_object (&self->search_cancellable);
    }
  g_clear_handle_id (&self->live_search_timer, g_source_remove);
  g_clear_handle_id (&self->history_timer, g_source_remove);

  if (self->favorites_filter)
    gtk_custom_filter_set_filter_func (self->favorites_filter, NULL, NULL, NULL);

  self->search_selected_action = NULL;
  g_clear_pointer (&self->completion, completion_free);

  gtk_widget_dispose_template (GTK_WIDGET (self), DICTIONARY_TYPE_WINDOW);
  G_OBJECT_CLASS (dictionary_window_parent_class)->dispose (obj);
}

static void
dictionary_window_finalize (GObject *obj)
{
  DictionaryWindow *self = DICTIONARY_WINDOW (obj);

  g_clear_object (&self->history_model);
  g_clear_object (&self->history_store);
  g_clear_object (&self->def_extra_menu);
  g_clear_pointer (&self->index, word_index_unref);
  if (self->wn)
    {
      base_wn_free (self->wn);
      self->wn = NULL;
    }
  g_free (self->lookup_term);
  g_free (self->searched_term);
  g_free (self->pending_history_text);

  G_OBJECT_CLASS (dictionary_window_parent_class)->finalize (obj);

  /* The main window is the only thing that needs the big word index and the
     large SQLite cache; once it is gone give every freed page back to the OS
     so a background/tray process drops to its idle footprint. */
  wb_release_memory ();
}

static void
dictionary_window_class_init (DictionaryWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = dictionary_window_dispose;
  object_class->finalize = dictionary_window_finalize;

  gtk_widget_class_set_template_from_resource (widget_class, WB_RES_PATH "/ui/window.ui");

#define BIND(id, field) \
  gtk_widget_class_bind_template_child_full (widget_class, id, FALSE, G_STRUCT_OFFSET (DictionaryWindow, field))
  BIND ("search_button", search_button);
  BIND ("title_clamp", title_clamp);
  BIND ("search_entry", search_entry);
  BIND ("split_view_toggle_button", split_view_toggle_button);
  BIND ("dictionary_menu_button", menu_button);
  BIND ("history_stack", history_stack);
  BIND ("history_listbox", history_listbox);
  BIND ("clear_history_button", clear_history_button);
  BIND ("favorites_filter_button", favorites_filter_button);
  BIND ("main_split_view", main_split_view);
  BIND ("main_stack", main_stack);
  BIND ("main_scroll", main_scroll);
  BIND ("toast_overlay", toast_overlay);
  BIND ("definitions_listbox", definitions_listbox);
  BIND ("search_fail_description_label", search_fail_description_label);
  BIND ("exit_button", exit_button);
  BIND ("choose_db_button", choose_db_button);
  BIND ("key_ctrlr", key_ctrlr);
  BIND ("welcome_words_box", welcome_words_box);
#undef BIND
}

static void
dictionary_window_init (DictionaryWindow *self)
{
  self->clear_icon_state = -1;
  gtk_widget_init_template (GTK_WIDGET (self));
}

DictionaryWindow *
dictionary_window_new (GtkApplication *app, const char *title, const char *term, gboolean auto_paste_requested)
{
  g_return_val_if_fail (GTK_IS_APPLICATION (app), NULL);

  DictionaryWindow *self = g_object_new (DICTIONARY_TYPE_WINDOW,
                                       "application", app,
                                       "title", title,
                                       NULL);

  gtk_window_set_default_size (GTK_WINDOW (self), cfg_window_width (), cfg_window_height ());

  self->lookup_term = g_strdup (term);
  self->auto_paste_requested = auto_paste_requested;

  gtk_window_set_default_icon_name (g_application_get_application_id (G_APPLICATION (app)));

  setup_actions (self);
  setup_widgets (self);
  return self;
}

void
dictionary_window_trigger_search (DictionaryWindow *self, const char *text)
{
  g_return_if_fail (DICTIONARY_IS_WINDOW (self));

  if (!text || utf8_is_blank (text))
    return;

  g_signal_handlers_block_by_func (self->search_entry, on_entry_changed, self);
  gtk_editable_set_text (GTK_EDITABLE (self->search_entry), text);
  gtk_editable_set_position (GTK_EDITABLE (self->search_entry), -1);
  g_signal_handlers_unblock_by_func (self->search_entry, on_entry_changed, self);

  start_search (self, text, TRUE);
}

void
dictionary_window_queue_auto_paste (DictionaryWindow *self)
{
  g_return_if_fail (DICTIONARY_IS_WINDOW (self));

  if (gtk_window_is_active (GTK_WINDOW (self)))
    g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, paste_idle_cb, g_object_ref (self), g_object_unref);
  else
    self->auto_paste_queued = TRUE;
}

void
dictionary_window_save_state (DictionaryWindow *self)
{
  g_return_if_fail (DICTIONARY_IS_WINDOW (self));

  g_clear_handle_id (&self->history_timer, g_source_remove);
  g_clear_handle_id (&self->live_search_timer, g_source_remove);

  GListModel *model = G_LIST_MODEL (self->history_store);
  guint n = g_list_model_get_n_items (model);
  const char **terms = g_new0 (const char *, n + 1);
  GPtrArray *keep = g_ptr_array_new_full (n, g_object_unref);

  for (guint i = 0; i < n; i++)
    {
      HistoryItem *item = g_list_model_get_item (model, i);
      g_ptr_array_add (keep, item);
      terms[i] = item->term;
    }

  int width, height;
  gtk_window_get_default_size (GTK_WINDOW (self), &width, &height);
  cfg_save_state (terms, width, height);

  g_ptr_array_unref (keep);
  g_free (terms);
}

void
dictionary_window_set_completion_enabled (DictionaryWindow *self, gboolean enabled)
{
  g_return_if_fail (DICTIONARY_IS_WINDOW (self));

  if (self->completion)
    completion_set_enabled (self->completion, enabled);
}

void
dictionary_window_refresh_current_search_pronunciations (DictionaryWindow *self)
{
  g_return_if_fail (DICTIONARY_IS_WINDOW (self));

  if (!self->searched_term)
    return;
  if (g_strcmp0 (adw_view_stack_get_visible_child_name (self->main_stack), PAGE_NAMES[PAGE_CONTENT]) != 0)
    return;

  g_autofree char *term = g_strdup (self->searched_term);
  start_search (self, term, FALSE);
}

GtkWidget *
dictionary_window_get_search_button (DictionaryWindow *self)
{
  g_return_val_if_fail (DICTIONARY_IS_WINDOW (self), NULL);

  return GTK_WIDGET (self->search_button);
}
