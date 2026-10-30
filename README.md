<div align="center">

<h1>Dictionary</h1>

<p><strong>The Windows-era dictionary lookup, reimagined native for GNOME.</strong></p>

<p>
A fast, offline GTK4 / libadwaita dictionary with live search, pronunciation audio and favorites,<br>
backed entirely by a local, compressed Open English WordNet database.
</p>

<p>
<img alt="Platform" src="https://img.shields.io/badge/platform-Linux-1f2328?style=flat-square">
<img alt="Toolkit" src="https://img.shields.io/badge/toolkit-GTK4%20%7C%20libadwaita-1f2328?style=flat-square">
<img alt="Built with C" src="https://img.shields.io/badge/built%20with-C%20(gnu11)-1f2328?style=flat-square">
<a href="https://github.com/shlok2721-web/Dictionary/releases/latest"><img alt="Latest Release" src="https://img.shields.io/github/v/release/shlok2721-web/Dictionary?style=flat-square&color=1f2328"></a>
<a href="LICENSE"><img alt="License" src="https://img.shields.io/badge/license-unspecified-1f2328?style=flat-square"></a>
</p>

<p>
<a href="#features">Features</a> &nbsp;·&nbsp;
<a href="#download-and-run">Download</a> &nbsp;·&nbsp;
<a href="#building-from-source">Build from Source</a> &nbsp;·&nbsp;
<a href="#keyboard-shortcuts">Shortcuts</a> &nbsp;·&nbsp;
<a href="#configuration">Configuration</a>
</p>

</div>

---

## Overview

**Dictionary** is a lightweight, offline-first lookup application built with **GTK4** and **libadwaita**, designed to feel at home on a modern GNOME desktop. It ships with a locally generated, Zstandard-compressed SQLite database built from the **Open English WordNet (2025 edition)**, so every lookup is instant and the app never needs a network connection to answer a query.

<table>
<tr>
<td width="33%" valign="top">

**Instant**<br>
Live search with debounced autocompletion, grouped results by lemma and pronunciation.

</td>
<td width="33%" valign="top">

**Native**<br>
A single GTK4 + libadwaita binary that follows your system's light/dark theme.

</td>
<td width="33%" valign="top">

**Offline**<br>
Definitions, synonyms, antonyms and pronunciations all come from a database on disk — no network calls at lookup time.

</td>
</tr>
</table>

![Dictionary main window](screenshots/main-window.png)

---

## Features

### Lookup & Results

| | |
|---|---|
| **Live search** | Debounced as-you-type search with a 400 ms delay, plus an inline completion dropdown |
| **Grouped results** | Definitions are grouped by lemma and then by pronunciation, so homographs with different pronunciations stay distinct |
| **Parts of speech** | Adjective, noun, verb, adverb, phrase, conjunction, adposition and other, each clearly labeled |
| **Rich entries** | Definitions, usage examples, synonyms, antonyms, similar words and "also see" cross-references |
| **Fuzzy suggestions** | A failed search still offers close matches, scored and ranked, instead of a dead end |
| **Search selected text** | Look up any text selected elsewhere on your system via `Ctrl+S` |
| **Random word** | Jump to a random dictionary entry for a bit of serendipity |

### Pronunciation

- **IPA transcriptions** shown alongside each entry, with a **fallback indicator** when an exact-form pronunciation isn't available
- **Spoken pronunciation** via text-to-speech, converting IPA to eSpeak phonemes
- **Accent choice** between **US** and **GB** pronunciation, set once in Preferences

### Favorites & History

- **Favorites** — save any term for one-tap access later
- **Search history** — your last 50 lookups are remembered automatically, and can be cleared at any time
- **Paste search** — paste your clipboard contents straight into search with `Ctrl+Shift+V`
- **Auto-paste on launch** — optionally have the app search your clipboard contents the moment it opens

### Interface

- Light and dark mode, following your system theme
- Collapsible sidebar (`Ctrl+B`) and app menu (`F10`)
- **Welcome screen with a few random words** — 2 or 3 definitions pulled live from the full WordNet index greet you on launch (and again each time you clear the search box), sized to fit the window
- Distinct states for a welcome screen, loading, a missing/broken database, and no results found
- Window size is remembered between sessions (defaults to 640×760, roomy enough for the welcome cards)
- **About dialog** with app info and a direct link back to this repository and its issue tracker

### Data & Database

- A **generator tool** (`generate_wn_db`) builds the lookup database directly from the official [Open English WordNet](https://github.com/globalwordnet/english-wordnet) XML release
- The resulting database is compressed with **Zstandard** to keep the shipped app small
- Powered by `oewn:2025+`, the 2025 edition of the Open English WordNet

---

## Keyboard Shortcuts

Every shortcut is `Ctrl`-based and chosen to stay clear of window-manager-level bindings (e.g. Hyprland's defaults, which live on the Super/Mod key) — nothing here should fight your compositor for a key combo.

| Action | Shortcut |
|---|---|
| Quit window | `Ctrl` `W` |
| Quit application | `Ctrl` `Q` |
| Preferences | `Ctrl` `P` |
| Search selected text | `Ctrl` `F` |
| Random word | `Ctrl` `R` |
| Paste and search | `Ctrl` `Shift` `V` |
| Toggle favorite | `Ctrl` `D` |
| Toggle sidebar | `Ctrl` `B` |
| Toggle app menu | `F10` *(GNOME HIG convention for the primary menu)* |

---

## Download and Run

Grab a prebuilt binary from the [**latest release**](https://github.com/shlok2721-web/Dictionary/releases/latest) and run it — no installation required.

### Requirements

- A Linux desktop running GTK4 and **libadwaita 1.6+** (needed for the synonym/antonym chip layout; most current, rolling-release distributions already ship this — check with `pkg-config --modversion libadwaita-1` if unsure)

---

## Building from Source

Clone the repository and build in four steps — compile the resources, build the app, build the database generator, and generate the dictionary data.

### Dependencies

| Dependency | Purpose |
|---|---|
| `gcc` (gnu11) | Compiler |
| `gtk4` | UI toolkit |
| `libadwaita-1` | GNOME adaptive widgets |
| `json-glib-1.0` | JSON parsing |
| `sqlite3` | Dictionary database engine |
| `libzstd` | Database compression |
| `glib-2.0` / `gio-2.0` | Core utilities & resource compiler |
| `libxml-2.0` | Parsing the WordNet XML source |

On Debian/Ubuntu-based systems:

```bash
sudo apt install build-essential pkg-config libgtk-4-dev libadwaita-1-dev \
  libjson-glib-dev libsqlite3-dev libzstd-dev libglib2.0-dev libxml2-dev
```

### Build steps

```bash
git clone https://github.com/shlok2721-web/Dictionary.git
cd Dictionary
mkdir -p build

# 1. Compile the UI resources
glib-compile-resources --sourcedir=data --target=build/resources.c --generate-source data/dev.silvanus.dictionary.gresource.xml

# 2. Build the app
gcc -std=gnu11 -O2 -Isrc \
  src/base.c src/database.c src/main.c src/search_completion.c src/settings.c \
  src/settings_window.c src/utils.c src/window.c build/resources.c \
  -o build/dictionary \
  $(pkg-config --cflags --libs gtk4 libadwaita-1 json-glib-1.0 sqlite3 libzstd) -lm

# 3. Build the database generator
gcc -std=gnu11 -O2 src/generate_wn_db.c -o build/generate_wn_db \
  $(pkg-config --cflags --libs glib-2.0 gio-2.0 sqlite3 libzstd libxml-2.0)

# 4. Generate the dictionary data (skip if data/wn-dev.db.zst already exists)
[ -f data/wn-dev.db.zst ] || {
  curl -L -o /tmp/ewn.xml.gz "https://github.com/globalwordnet/english-wordnet/releases/download/2025-edition/english-wordnet-2025-plus.xml.gz"
  build/generate_wn_db --source-file /tmp/ewn.xml.gz --output data/wn-dev.db.zst
}

# 5. Run
MESON_BUILD_ROOT=$PWD build/dictionary
```

---

## Project Structure

```
dictionary/
├── build/                    # Build output (generated)
│   ├── dictionary             # Compiled application binary
│   ├── generate_wn_db          # Database generator tool
│   └── resources.c             # Compiled GResource bundle
├── data/
│   ├── dev.silvanus.dictionary.gresource.xml
│   ├── style.css               # Application styling
│   ├── ui/
│   │   ├── settings-window.ui
│   │   └── window.ui
│   └── wn-dev.db.zst           # Compressed WordNet database
└── src/
    ├── base.c / base.h             # Core dictionary & definition logic
    ├── constants.h                 # Shared constants and part-of-speech enum
    ├── database.c / database.h     # SQLite access layer
    ├── generate_wn_db.c            # WordNet XML → SQLite database generator
    ├── main.c                      # Application entry point & global actions
    ├── search_completion.c / .h    # Live search / autocomplete
    ├── settings.c / .h             # Preferences, history & favorites storage
    ├── settings_window.c / .h      # Preferences dialog
    ├── utils.c / .h                # Shared utilities
    └── window.c / .h               # Main application window
```

---

## Configuration

Preferences (live search, double-click behavior, pronunciation accent, auto-paste on launch, dark mode), search history and favorites are all managed from the in-app **Preferences** window (`Ctrl+,`) — no manual config editing required.

---

## Current Limitations

- Linux only, built against GTK4 and libadwaita
- English only, sourced from the Open English WordNet
- Single, bundled dictionary database — no support yet for additional or custom data sources

---

## License

This project's license has not been finalized yet — add a `LICENSE` file to the repository root and update this section (and the badge above) once chosen.

---

## About

The app's in-app **About** dialog (from the app menu, `F10`) links straight back here and to the [issue tracker](https://github.com/shlok2721-web/Dictionary/issues) — so feedback and bug reports are always one click away, no matter where someone got the build.

## Acknowledgments

- [Open English WordNet](https://github.com/globalwordnet/english-wordnet) — the lexical database powering every definition
- [GTK](https://www.gtk.org/) & [libadwaita](https://gnome.pages.gitlab.gnome.org/libadwaita/) — the native GNOME application framework

<div align="center">
<br>
<sub>Dictionary. Every word, right where you work.</sub>
</div>
