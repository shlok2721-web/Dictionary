#pragma once

#include <glib.h>

G_BEGIN_DECLS

#ifndef WN_FILE_VERSION
#define WN_FILE_VERSION "dev"
#endif

#define WB_RES_PATH "/dev/silvanus/dictionary"

#define WB_WN_DB_VERSION "oewn:2025+"

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
  WB_POS_OTHER,
  WB_POS_UNKNOWN,
  WB_POS_COUNT
} WbPos;

static inline const char *
wb_pos_name (WbPos pos)
{
  static const char *const names[WB_POS_COUNT] = {
    "adjective", "noun", "verb", "adverb", "phrase",
    "conjunction", "adposition", "other", "unknown",
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

G_END_DECLS
