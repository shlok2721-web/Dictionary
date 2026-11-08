#!/usr/bin/env python3
"""Build a small synthetic Krumeto-schema SQLite database for CI.

Only used when no real database is available (see the workflow).  It has every
table/column the app queries, so the app opens, prepares all statements and
renders entries, but it is NOT real dictionary data.

usage: make_test_db.py OUTPUT.db [N_ENTRIES]
"""
import json, os, random, sqlite3, sys

out = sys.argv[1]
n = int(sys.argv[2]) if len(sys.argv) > 2 else 20000
if os.path.exists(out):
    os.remove(out)
random.seed(1)
db = sqlite3.connect(out)
db.executescript("""
CREATE TABLE entries(id INTEGER PRIMARY KEY, lemma TEXT COLLATE NOCASE, pos TEXT, cefr TEXT,
  cefr_pos TEXT, zipf REAL, freq_bucket TEXT, hyphenation TEXT, etymology TEXT, usage_notes TEXT,
  wiki_url TEXT, transitive INTEGER, countable INTEGER, pos_rank INTEGER);
CREATE INDEX idx_entries_lemma ON entries(lemma COLLATE NOCASE);
CREATE TABLE form_index(form TEXT, entry_id INTEGER);
CREATE INDEX idx_form ON form_index(form);
CREATE TABLE senses(id INTEGER PRIMARY KEY, entry_id INTEGER, ord INTEGER, rank INTEGER,
  gloss TEXT, parent_gloss TEXT, tags TEXT);
CREATE INDEX idx_senses ON senses(entry_id);
CREATE TABLE sense_examples(sense_id INTEGER, text TEXT);
CREATE INDEX idx_sex ON sense_examples(sense_id);
CREATE TABLE pron(entry_id INTEGER, ipa TEXT, variety TEXT, audio_url TEXT, audio_credit TEXT, audio_license TEXT);
CREATE INDEX idx_pron ON pron(entry_id);
CREATE TABLE links(entry_id INTEGER, kind TEXT, target TEXT, sense_id INTEGER);
CREATE INDEX idx_links ON links(entry_id);
CREATE TABLE lemma_rel(src TEXT, src_pos TEXT, rel TEXT, dst TEXT);
CREATE INDEX idx_lrel ON lemma_rel(src, src_pos);
CREATE TABLE suggest(term TEXT, kind TEXT);
CREATE TABLE forms(entry_id INTEGER, form TEXT, tags TEXT);
CREATE INDEX idx_forms ON forms(entry_id);
CREATE TABLE collocations(lemma TEXT, pattern TEXT, count INTEGER);
CREATE INDEX idx_coll ON collocations(lemma);
CREATE TABLE example_top(lemma TEXT, sentence_id INTEGER, ord INTEGER);
CREATE INDEX idx_et ON example_top(lemma);
CREATE TABLE sentences(id INTEGER PRIMARY KEY, text TEXT);
CREATE TABLE sources(name TEXT, version TEXT, license TEXT, url TEXT);
CREATE TABLE license_notice(name TEXT, text TEXT);
CREATE TABLE meta(key TEXT, value TEXT);
""")
syl = ["ba","ro","ti","mel","dor","an","ek","lu","sa","vin","por","gle","han","mi","zen"]
poses = ["noun", "verb", "adj", "adv"]
words = set()
while len(words) < n:
    words.add("".join(random.choice(syl) for _ in range(random.randint(2, 4))))
words = sorted(words)
sid = 0
for i, w in enumerate(words, start=1):
    pos = poses[i % 4]
    db.execute("INSERT INTO entries VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
               (i * 3, w, pos, "B1", "B1", 4.0, "common", w, "From synthetic roots.",
                "Test usage note.", "https://example.org/" + w, 1, 1, 0))
    eid = i * 3
    db.execute("INSERT INTO suggest VALUES(?,?)", (w, "lemma"))
    db.execute("INSERT INTO form_index VALUES(?,?)", (w + "s", eid))
    db.execute("INSERT INTO forms VALUES(?,?,?)", (eid, w + "s", json.dumps(["plural"])))
    db.execute("INSERT INTO pron VALUES(?,?,?,?,?,?)", (eid, "/" + w + "/", "us", "", "", ""))
    for k in range(2):
        sid += 1
        db.execute("INSERT INTO senses VALUES(?,?,?,?,?,?,?)",
                   (sid, eid, k, k, f"A synthetic meaning {k} of {w}.", None, json.dumps(["test"])))
        db.execute("INSERT INTO sense_examples VALUES(?,?)", (sid, f"This is an example using {w}."))
    other = words[(i * 7) % len(words)]
    db.execute("INSERT INTO links VALUES(?,?,?,?)", (eid, "syn", other, sid))
    db.execute("INSERT INTO links VALUES(?,?,?,?)", (eid, "hypernym", other, sid))
    db.execute("INSERT INTO collocations VALUES(?,?,?)", (w, f"take {w} into account", 5))
    db.execute("INSERT INTO sentences VALUES(?,?)", (i, f"A corpus sentence about {w}."))
    db.execute("INSERT INTO example_top VALUES(?,?,?)", (w, i, 0))
db.execute("INSERT INTO sources VALUES('Synthetic CI data','0','CC0','https://example.org')")
db.execute("INSERT INTO license_notice VALUES('Synthetic','Generated for CI only.')")
db.execute("INSERT INTO meta VALUES('generator','make_test_db.py')")
db.commit(); db.close()
print(f"wrote {out} ({n} entries)")
