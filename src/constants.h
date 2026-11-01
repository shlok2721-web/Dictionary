#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Version of the bundled Krumeto database (the date in its release file name,
 * e.g. krumeto-data-2026-10-25.sqlite).  The name WN_FILE_VERSION is kept so
 * existing build scripts that grep for it keep working. */
#ifndef WN_FILE_VERSION
#define WN_FILE_VERSION "2026-10-25"
#endif

/* The database is shipped uncompressed and opened in place:
 *   <data dir>/krumeto-<version>.db            (per-user install)
 *   <system data dir>/dictionary/krumeto-<version>.db   (system install)
 * WB_DB_DIR_PREFIX is only used to recognise (and remove) the extracted
 * krumeto-<version>/ copies that older versions created. */
#define WB_DB_DIR_PREFIX   "krumeto-"
#define WB_DB_FILE_FMT     "krumeto-%s.db"

#define WB_RES_PATH "/dev/silvanus/dictionary"

#define WB_DARK_MODE_SENTENCE_COLOR  "cyan"
#define WB_LIGHT_MODE_SENTENCE_COLOR "blue"

#define WB_SEARCH_TERM_CLEANUP_CHARS "<>\"-?`![](){}/:;,"
#define WB_SEARCH_TERM_REPLACE_CHARS "()<>[]&\\\n"

typedef enum {
  WB_POS_ADJECTIVE = 0,
  WB_POS_NOUN,
  WB_POS_VERB,
  WB_POS_ADVERB,
  WB_POS_PHRASE,
  WB_POS_CONJUNCTION,
  WB_POS_ADPOSITION,
  WB_POS_PROPER_NOUN,
  WB_POS_INTERJECTION,
  WB_POS_PRONOUN,
  WB_POS_NUMERAL,
  WB_POS_DETERMINER,
  WB_POS_PARTICLE,
  WB_POS_CONTRACTION,
  WB_POS_OTHER,
  WB_POS_UNKNOWN,
  WB_POS_COUNT
} WbPos;

static inline const char *
wb_pos_name (WbPos pos)
{
  static const char *const names[WB_POS_COUNT] = {
    "adjective", "noun", "verb", "adverb", "phrase",
    "conjunction", "adposition",
    "proper noun", "interjection", "pronoun", "numeral",
    "determiner", "particle", "contraction",
    "other", "unknown",
  };
  return (pos >= 0 && pos < WB_POS_COUNT) ? names[pos] : "unknown";
}

static inline gboolean
wb_pos_from_tag (const char *tag, WbPos *out)
{
  *out = WB_POS_UNKNOWN;
  if (tag == NULL || tag[0] == '\0' || tag[1] != '\0')
    return FALSE;
  switch (tag[0]) {
    case 's': case 'a': *out = WB_POS_ADJECTIVE;    return TRUE;
    case 'n':           *out = WB_POS_NOUN;         return TRUE;
    case 'v':           *out = WB_POS_VERB;         return TRUE;
    case 'r':           *out = WB_POS_ADVERB;       return TRUE;
    case 't':           *out = WB_POS_PHRASE;       return TRUE;
    case 'c':           *out = WB_POS_CONJUNCTION;  return TRUE;
    case 'p':           *out = WB_POS_ADPOSITION;   return TRUE;
    case 'x':           *out = WB_POS_OTHER;        return TRUE;
    case 'u':           *out = WB_POS_UNKNOWN;      return TRUE;
    default:            return FALSE;
  }
}

/* Krumeto's entries.pos vocabulary (full words, not WordNet's one-letter tags):
 * noun adj name verb adv phrase intj prep pron num contraction conj det particle */
static inline gboolean
wb_pos_from_krumeto_tag (const char *pos, WbPos *out)
{
  static const struct { const char *tag; WbPos pos; } map[] = {
    { "noun",        WB_POS_NOUN        }, { "adj",      WB_POS_ADJECTIVE   },
    { "name",        WB_POS_PROPER_NOUN }, { "verb",     WB_POS_VERB        },
    { "adv",         WB_POS_ADVERB      }, { "phrase",   WB_POS_PHRASE      },
    { "intj",        WB_POS_INTERJECTION}, { "prep",     WB_POS_ADPOSITION  },
    { "pron",        WB_POS_PRONOUN     }, { "num",      WB_POS_NUMERAL     },
    { "contraction", WB_POS_CONTRACTION }, { "conj",     WB_POS_CONJUNCTION },
    { "det",         WB_POS_DETERMINER  }, { "particle", WB_POS_PARTICLE    },
  };
  *out = WB_POS_UNKNOWN;
  if (pos == NULL)
    return FALSE;
  for (gsize i = 0; i < G_N_ELEMENTS (map); i++)
    if (g_strcmp0 (pos, map[i].tag) == 0) {
      *out = map[i].pos;
      return TRUE;
    }
  *out = WB_POS_OTHER;
  return FALSE;
}

G_END_DECLS
