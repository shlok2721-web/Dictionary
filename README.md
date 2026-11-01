<div align="center">

<h1>Dictionary</h1>

<p><strong>The Windows-era dictionary lookup, reimagined native for GNOME.</strong></p>

<p>
A fast, offline GTK4 / libadwaita dictionary with live search, pronunciation audio and favorites,<br>
backed entirely by a local Krumeto dictionary database.
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

**Dictionary** is a lightweight, offline-first lookup application built with **GTK4** and **libadwaita**, designed to feel at home on a modern GNOME desktop. It ships with a locally generated SQLite database built from the **Krumeto** dictionary data (headwords, multi-sense glosses, CEFR levels, etymology, IPA, relations), so every lookup is instant and the app never needs a network connection to answer a query.

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
- **eSpeak fallback** — Dictionary uses the `espeak-ng` command from your `PATH`. If that command isn't installed it loads `libespeak-ng` directly instead (the system copy first, then any location listed in `EXTRA_LIB_GLOBS` in `src/espeak_lib.c`, which by default includes a Python virtualenv's `espeakng_loader` at `~/myenv/lib/python3*/site-packages/espeakng_loader/`). You can also point to a specific library with the `WB_ESPEAK_LIBRARY` (or `PHONEMIZER_ESPEAK_LIBRARY`) environment variable. In library mode, speech is played through the first available of `pw-play`, `paplay`, `aplay` or `ffplay`
- **Accent choice** between **US** and **GB** pronunciation, set once in Preferences
- **Voice** — Female (default), Female (Soft), Male or espeak-ng's own flat default voice, plus a **Pitch** slider (0-99, 50 = unchanged), both in Preferences
- **Recorded pronunciations** — when a dictionary entry has one and **Prefer Recorded Pronunciations** is turned on in Preferences, Dictionary plays that human recording instead of synthesizing speech, downloading and caching it under the data directory the first time it's played. This is **off by default** and is the only thing in Dictionary that touches the network — everything else, including the normal "no network calls at lookup time" claim above, still holds

### Favorites & History

- **Favorites** — save any term for one-tap access later
- **Search history** — your last 50 lookups are remembered automatically, and can be cleared at any time
- **Paste search** — paste your clipboard contents straight into search with `Ctrl+Shift+V`
- **Auto-paste on launch** — optionally have the app search your clipboard contents the moment it opens

### Tray, Background Service & Clipboard Lookup

- **Tray icon** via the StatusNotifierItem protocol — shows up in any SNI-compatible host, including **waybar**'s tray module, GNOME Shell's AppIndicator extension and Plasma's system tray. **Left-click turns automatic word lookup on or off**; the icon switches between an *active* look (`dict-tray.svg`) and a grey *inactive* look (`dict-tray-inactive.svg`), and the tooltip says which state you're in. Both icons are compiled into the binary (resources `tray/dict-tray.svg` and `tray/dict-tray-inactive.svg`) and exported at startup to `$XDG_RUNTIME_DIR/dict-tray/`, which the tray advertises via `IconThemePath`, so waybar finds them without any install step or icon-cache refresh. Clicking the icon never opens the mini window by itself. **Middle-click** opens the main window; **right-click** opens a small menu (Open Dictionary, Automatic Word Lookup, Keep Running in Background, Quit)
- **Keep Running in Background** — closing the window no longer has to quit the app; it keeps running (tray icon and/or clipboard watcher included) until you quit it explicitly
- **Automatic Word Lookup** — with lookup on, **one or two words** you copy *or merely highlight* anywhere on your system open the mini window with the definition, with no network access involved (same local Krumeto database as the rest of the app). Longer text, multi-line text, URLs, paths and anything without letters is ignored. **Preferences → Lookup Trigger** chooses what counts:
  - **Copied text** (default): the clipboard, i.e. after Ctrl+C
  - **Selected (highlighted) text**: the primary selection, so just highlighting a word with the mouse is enough, with no copy needed. Rapid selection changes while you drag are coalesced (about 0.2 s), so the popup appears once the selection settles
  - **Copied or selected text**: both. Highlighting a word and then copying that same word within about 1.5 s opens the popup only once

  Looking up the same word again later works as normal; only an identical repeat within about 1.5 s is treated as one gesture. Turning lookup on never pops up whatever was already selected or copied beforehand.

  The setting is shared by the tray icon, the tray menu checkmark and Preferences, so they always agree; changing the trigger takes effect immediately, no restart needed. Both modes need `wl-clipboard` on Wayland (see below). The choice is stored as `tray.lookup_source` (`"clipboard"`, `"selection"` or `"both"`) in `dictionary.json`; configs from before this option existed keep the default
- **Mini lookup window** — a small, borderless popup anchored to the top-right corner of your screen, sized as a percentage of your monitor's resolution (20% by default, adjustable in Preferences). A popup triggered by a copy hides itself after 5 seconds; moving the mouse over it restarts the countdown, and the countdown starts only once the definition has loaded. Popups you open yourself ("View Full Entry", etc.) stay until closed
- **`--background` launch flag** — start Dictionary straight into tray/service mode without opening the main window at all, e.g. from a systemd user service (see below). It only suppresses the window for that first launch; later plain `dictionary` launches open the window normally

All of these are independent toggles under **Preferences → Tray & Background**, off by default except the tray icon itself (automatic lookup in particular is opt-in, since it means reading your clipboard or selection contents — so the tray icon starts out in its inactive look until you click it once).

> The tray icon depends on an SNI-compatible host being present (waybar, GNOME Shell + AppIndicator extension, Plasma, etc.). On a desktop without one, the icon simply never appears — nothing else in the app is affected, and "Keep Running in Background" still works via `--background` or the Preferences toggle even with the tray off.

> **Wayland clipboard watching needs `wl-clipboard`.** On Wayland a compositor only tells the *focused* window that the clipboard changed, so a background/tray app would never notice copies made elsewhere. Dictionary therefore uses `wl-paste --watch` (the data-control protocol, supported by Hyprland, Sway and other wlroots compositors, and KDE) when it's installed, and falls back to GTK's own clipboard signal — which only sees copies made while Dictionary's own window is focused — when it isn't. Install it with `sudo pacman -S wl-clipboard` or `sudo apt install wl-clipboard`.

> **Only one instance can run.** Dictionary is a single-instance application: if one is already running, launching it again just forwards the request to that process and exits, so new builds or `-v` output appear to do nothing. After rebuilding, stop the old one first with `pkill -f '.local/bin/dictionary'` (check with `busctl --user list | grep -i dictionary`), then run `dictionary-launcher -v`.

#### Running as a persistent service

To have Dictionary's tray icon and clipboard watcher start automatically at login (independent of opening the window), install the included systemd user unit:

```bash
mkdir -p ~/.config/systemd/user
cp packaging/dictionary-tray.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now dictionary-tray.service
```

This runs `dictionary --background`, which turns on "Keep Running in Background" and starts without opening a window. Enable "Automatic Word Lookup" from Preferences (or the tray's right-click menu) once, and it stays on for future launches of the service.

> **Hyprland / Sway / wlroots compositors:** systemd user services only see `WAYLAND_DISPLAY` and `DBUS_SESSION_BUS_ADDRESS` if your compositor imports them into the systemd user environment on startup. If the service starts but the tray icon or popup never appear, add a line like `exec-once = systemctl --user import-environment WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS XDG_CURRENT_DESKTOP` (Hyprland) or the Sway equivalent to your compositor config, then `systemctl --user restart dictionary-tray.service`.

### Interface

- Light and dark mode, following your system theme
- Collapsible sidebar (`Ctrl+B`) and app menu (`F10`)
- **Welcome screen with a few random words** — 2 or 3 definitions pulled live from the full Krumeto word index greet you on launch (and again each time you clear the search box), sized to fit the window
- Distinct states for a welcome screen, loading, a missing/broken database, and no results found
- Window size is remembered between sessions (defaults to 640×760, roomy enough for the welcome cards)
- **About dialog** with app info and a direct link back to this repository and its issue tracker

### Data & Database

- The full Krumeto SQLite file is shipped as-is and **opened directly in place** (read-only, immutable) — there is no decompression step, no first-run extraction and no second copy on disk; nothing is trimmed, so every entry, sense, example, pronunciation and relation is available
- Extracted copies (`krumeto-<version>/`) left behind by older versions are removed automatically on first launch
- **First-run prompt** — if no database is found in any of the usual places, Dictionary asks where it is (a file chooser, with validation) and starts using it immediately, no restart needed. The error page keeps a **Choose Database…** button if you dismiss the prompt
- **Custom database location** — in **Preferences → Database** you can point Dictionary at any `krumeto-*.db` / SQLite file on your machine with **Choose…** (the file is checked first: it must be a real SQLite database containing the tables Dictionary needs). The reset button returns to automatic lookup. The change takes effect the next time Dictionary starts, and if the chosen file later goes missing Dictionary falls back to the default location instead of failing
- Queries Krumeto's native schema directly (`entries`, `senses`, `form_index`, `pron`, `links`, `lemma_rel`, `suggest`)

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
| `glib-2.0` / `gio-2.0` | Core utilities & resource compiler |
| `gtk4-layer-shell-0` | Anchors the mini lookup popup to a screen corner on Wayland |
| `espeak-ng` (runtime, optional) | Pronunciations and speech; if the command is missing, `libespeak-ng` is loaded as a fallback |
| `wl-clipboard` (runtime, optional) | Lets the clipboard watcher see copies made while Dictionary is in the background on Wayland |

The tray icon itself (`tray.c`) is implemented directly over GDBus (part of `gio-2.0`, already a dependency) — no separate tray/appindicator library needed.

> **`gtk4-layer-shell` vs `gtk-layer-shell` — these are two different projects, not two names for the same thing.** [`gtk-layer-shell`](https://github.com/wmww/gtk-layer-shell) (no "4") is the original library, built against **GTK3**. [`gtk4-layer-shell`](https://github.com/wmww/gtk4-layer-shell) is a separate, GTK4-only rewrite with its own pkg-config name, **`gtk4-layer-shell-0`**. This app needs the GTK4 one. If both happen to be installed and the build links the wrong one in, the binary mixes GTK3 and GTK4 symbols and aborts immediately on launch with `Gtk-ERROR: GTK 2/3 symbols detected` — so get the package name exactly right below, not whichever `*layer-shell*` package your distro suggests first.

On Arch Linux and Arch-based distributions, the correct package is in the official `extra` repo:
```bash
sudo pacman -S --needed base-devel pkgconf gtk4 libadwaita json-glib sqlite glib2 gtk4-layer-shell wl-clipboard
```

On Debian/Ubuntu-based systems, there is no official `gtk4-layer-shell` package as of this writing — only the GTK3 `libgtk-layer-shell-dev` is in the repos, and that's the one to avoid. Install everything else from apt, then build `gtk4-layer-shell` itself from source:
```bash
sudo apt install build-essential pkg-config meson ninja-build libgtk-4-dev libadwaita-1-dev \
  libjson-glib-dev libsqlite3-dev libglib2.0-dev wl-clipboard \
  libwayland-dev wayland-protocols gobject-introspection libgirepository1.0-dev

git clone https://github.com/wmww/gtk4-layer-shell.git /tmp/gtk4-layer-shell
cd /tmp/gtk4-layer-shell
meson setup build -Dexamples=false -Ddocs=false -Dtests=false
ninja -C build
sudo ninja -C build install
sudo ldconfig
cd -
```
This installs `gtk4-layer-shell-0.pc`, `libgtk4-layer-shell.so`, and the `<gtk4-layer-shell/gtk4-layer-shell.h>` header this project includes, under `/usr/local` (`ninja install`'s default prefix) without touching the GTK3 `gtk-layer-shell` package, if you have it installed for something else.

On Fedora, `sudo dnf install gtk4-layer-shell-devel` provides the correct package directly.

`gtk4-layer-shell` also needs to appear *before* `gtk4` in the linker's package list (a static-initializer ordering requirement the upstream project documents) — the build script below already does this; keep it that way if you adjust the `pkg-config --cflags --libs` call.

### Krumeto database (download and install)

The app needs the Krumeto SQLite database in `~/.local/share/dictionary/` (the standard per-user data directory, `$XDG_DATA_HOME/dictionary`), named `krumeto-<version>.db`. The version the app expects is `WN_FILE_VERSION` in `src/constants.h` (currently `2026-10-25`, so `krumeto-2026-10-25.db`).

The build script below does all of this for you. If you want to do it by hand:

```bash
# Official Krumeto download link
export KRUMETO_URL='https://krumeto.com/dumps/krumeto-data-2026-10-25.sqlite.zst'
export KRUMETO_VERSION='2026-10-25'

mkdir -p ~/.local/share/dictionary
cd "$(mktemp -d)"
curl -fL --retry 3 -o krumeto.download "$KRUMETO_URL"

# Uncompress - use the line matching the file you downloaded:
#   zstd -d krumeto.download -o krumeto.sqlite     # .zst
#   gzip -dc krumeto.download > krumeto.sqlite     # .gz
#   xz -dc krumeto.download > krumeto.sqlite       # .xz
#   unzip krumeto.download                         # .zip
# (a plain .sqlite file needs no uncompressing: mv krumeto.download krumeto.sqlite)

# Check it is a real SQLite file, then install it under the name the app expects
head -c 15 krumeto.sqlite; echo
install -Dm644 krumeto.sqlite ~/.local/share/dictionary/krumeto-$KRUMETO_VERSION.db
```

If you keep the database somewhere else, point the app at it under **Preferences → Database → Choose…**.

### Build steps

Set `KRUMETO_URL` to the official Krumeto download link first (`export KRUMETO_URL='https://...'`), or edit the placeholder at step 10. The script downloads and uncompresses the database for you, unless it is already installed. Then paste the whole block into your terminal (or save it as a script and `source` it). It is wrapped in a function and uses `return`, never `exit`, so if a step fails it stops with an error message and **your terminal stays open**.

```bash

# ============================================================
# Dictionary - Build and Install
# ============================================================

build_dictionary() {

# 1. Clone the repository
git clone https://github.com/shlok2721-web/Dictionary.git
cd Dictionary

# 2. Locate the repository
ROOT="$(git rev-parse --show-toplevel 2>/dev/null)"

# If not inside a Git checkout, search nearby recent downloads.
# The 20-minute timestamp is a fallback, not the primary locator.
if [ -z "$ROOT" ]; then
    for SEARCH_DIR in "$PWD" "$HOME/Downloads" "$HOME/ramdisk/Downloads"; do
        [ -d "$SEARCH_DIR" ] || continue

        MAIN_FILE="$(find "$SEARCH_DIR" \
            -maxdepth 6 -type f \
            -path '*/src/main.c' \
            -mmin -20 -print -quit 2>/dev/null)"

        if [ -n "$MAIN_FILE" ]; then
            ROOT="$(dirname "$(dirname "$MAIN_FILE")")"
            break
        fi
    done
fi

[ -n "$ROOT" ] || {
    echo "ERROR: Dictionary repository not found."
    return 1
}

cd "$ROOT" || return 1
ROOT="$(pwd -P)"

echo "Repository: $ROOT"

# 3. Verify repository structure
REQUIRED_FILES=(
    src/main.c
    src/database.c
    src/espeak_lib.c
    src/constants.h
    src/utils.c
    src/tray.c
    src/clipboard_watch.c
    src/popup_window.c
    data/dev.silvanus.dictionary.gresource.xml
)

for ITEM in "${REQUIRED_FILES[@]}"; do
    if [ ! -s "$ROOT/$ITEM" ]; then
        echo "ERROR: Missing required file: $ITEM"
        return 1
    fi
done

# 4. Discover application icon
ICON_SOURCE="$(find "$ROOT" -type f \
    \( -name 'dictionary_icon.svg' \
       -o -name 'dev.silvanus.dictionary.svg' \) \
    -print -quit 2>/dev/null)"

if [ -z "$ICON_SOURCE" ] || [ ! -s "$ICON_SOURCE" ]; then
    echo "ERROR: Application SVG icon not found."
    echo "Expected dictionary_icon.svg somewhere inside the repository."
    return 1
fi

if ! grep -qi '<svg' "$ICON_SOURCE"; then
    echo "ERROR: Discovered icon is not a valid SVG document."
    return 1
fi

echo "Icon discovered: $ICON_SOURCE"

# 5. Read database version directly from source
WN_VERSION="$(sed -n 's/^#define[[:space:]]\+WN_FILE_VERSION[[:space:]]\+"\([^"]*\)".*/\1/p' \
    "$ROOT/src/constants.h" | head -n 1)"

[ -n "$WN_VERSION" ] || {
    echo "ERROR: Cannot determine database version."
    return 1
}

DB_NAME="krumeto-${WN_VERSION}.db"

echo "Database version: $WN_VERSION"
echo "Expected database: $DB_NAME"

# 6. Check build dependencies
for CMD in gcc pkg-config glib-compile-resources curl find install; do
    command -v "$CMD" >/dev/null 2>&1 || {
        echo "ERROR: Missing command: $CMD"
        return 1
    }
done

pkg-config --exists \
    gtk4 libadwaita-1 json-glib-1.0 sqlite3 \
    glib-2.0 gio-2.0 gtk4-layer-shell-0 || {
        echo "ERROR: Required development libraries are missing."
        return 1
    }

# 7. Prepare build directory
BUILD_DIR="$ROOT/build"
mkdir -p "$BUILD_DIR" || return 1

# 8. Compile UI resources
echo "Compiling UI resources..."

glib-compile-resources \
    --sourcedir="$ROOT/data" \
    --target="$BUILD_DIR/resources.c" \
    --generate-source \
    "$ROOT/data/dev.silvanus.dictionary.gresource.xml" || return 1

[ -s "$BUILD_DIR/resources.c" ] || {
    echo "ERROR: Resource compilation failed."
    return 1
}

# 9. Compile application
echo "Compiling Dictionary..."

gcc -std=gnu11 -O2 -Isrc \
    src/base.c \
    src/clipboard_watch.c \
    src/database.c \
    src/espeak_lib.c \
    src/main.c \
    src/popup_window.c \
    src/search_completion.c \
    src/settings.c \
    src/settings_window.c \
    src/tray.c \
    src/utils.c \
    src/window.c \
    "$BUILD_DIR/resources.c" \
    -o "$BUILD_DIR/dictionary" \
    $(pkg-config --cflags --libs \
        gtk4-layer-shell-0 gtk4 libadwaita-1 json-glib-1.0 sqlite3) \
    -ldl -lm || return 1

# Verify actual binary
if [ ! -x "$BUILD_DIR/dictionary" ] || [ ! -s "$BUILD_DIR/dictionary" ]; then
    echo "ERROR: Compiled application binary is missing."
    return 1
fi

echo "Application binary verified."

# 10. Obtain the Krumeto database (installed as-is, never compressed)
#
# Order of preference:
#   a) already installed in ~/.local/share/dictionary  -> reuse it
#   b) a local copy (repo data/ folder, or $KRUMETO_SQLITE)
#   c) download from $KRUMETO_URL and uncompress it
DATA_DIR="$HOME/.local/share/dictionary"
mkdir -p "$DATA_DIR" || return 1

# >>> Put the official Krumeto download link here (or export KRUMETO_URL first) <<<
KRUMETO_URL="${KRUMETO_URL:-PASTE_OFFICIAL_KRUMETO_DOWNLOAD_URL_HERE}"

is_sqlite() { [ -s "$1" ] && [ "$(head -c 15 "$1" 2>/dev/null)" = "SQLite format 3" ]; }

DB_SOURCE=""
DL_DIR=""

if is_sqlite "$DATA_DIR/$DB_NAME"; then
    DB_SOURCE="$DATA_DIR/$DB_NAME"
    echo "Database already installed: $DB_SOURCE"
else
    for CAND in \
        "$(find "$ROOT/data" -maxdepth 2 -type f -name "$DB_NAME" -print -quit 2>/dev/null)" \
        "${KRUMETO_SQLITE:-$ROOT/krumeto-data-${WN_VERSION}.sqlite}"; do
        if is_sqlite "$CAND"; then DB_SOURCE="$CAND"; break; fi
    done
fi

if [ -z "$DB_SOURCE" ]; then
    case "$KRUMETO_URL" in
        http://*|https://*) ;;
        *)
            echo "ERROR: No Krumeto database found and no download link set."
            echo "Set the official link and run again:"
            echo "    export KRUMETO_URL='https://...'"
            echo "or place krumeto-data-${WN_VERSION}.sqlite in the repo root."
            return 1 ;;
    esac

    echo "Downloading Krumeto database..."
    DL_DIR="$(mktemp -d)" || return 1
    DL_FILE="$DL_DIR/$(basename "${KRUMETO_URL%%\?*}")"

    curl -fL --retry 3 --retry-delay 2 --progress-bar -o "$DL_FILE" "$KRUMETO_URL" || {
        echo "ERROR: Download failed: $KRUMETO_URL"
        rm -rf "$DL_DIR"; return 1
    }

    # Uncompress according to the file name; plain .sqlite/.db needs nothing.
    echo "Uncompressing..."
    case "$DL_FILE" in
        *.zst)         command -v zstd  >/dev/null || { echo "ERROR: install zstd";  rm -rf "$DL_DIR"; return 1; }
                       zstd -dq --rm "$DL_FILE" -o "${DL_FILE%.zst}" ;;
        *.gz)          gzip -d "$DL_FILE" ;;
        *.xz)          xz -d "$DL_FILE" ;;
        *.bz2)         bzip2 -d "$DL_FILE" ;;
        *.zip)         command -v unzip >/dev/null || { echo "ERROR: install unzip"; rm -rf "$DL_DIR"; return 1; }
                       unzip -qo "$DL_FILE" -d "$DL_DIR" && rm -f "$DL_FILE" ;;
        *.tar.gz|*.tgz) tar -xzf "$DL_FILE" -C "$DL_DIR" && rm -f "$DL_FILE" ;;
    esac || { echo "ERROR: Uncompress failed."; rm -rf "$DL_DIR"; return 1; }

    # Pick the SQLite file that came out (whatever it is called).
    while IFS= read -r CAND; do
        if is_sqlite "$CAND"; then DB_SOURCE="$CAND"; break; fi
    done < <(find "$DL_DIR" -type f -size +1M 2>/dev/null)

    [ -n "$DB_SOURCE" ] || {
        echo "ERROR: The download did not contain a SQLite database."
        rm -rf "$DL_DIR"; return 1
    }
fi

echo "Database verified: $DB_SOURCE"

# 12. Prepare user installation directories
BIN_DIR="$HOME/.local/bin"
DATA_DIR="$HOME/.local/share/dictionary"
ICON_DIR="$HOME/.local/share/icons/hicolor/scalable/apps"
APP_DIR="$HOME/.local/share/applications"
CONFIG_DIR="$HOME/.config/dictionary"

mkdir -p "$BIN_DIR" "$DATA_DIR" "$ICON_DIR" \
    "$APP_DIR" "$CONFIG_DIR" || return 1

# 13. Install executable
install -Dm755 \
    "$BUILD_DIR/dictionary" \
    "$BIN_DIR/dictionary" || return 1

# 14. Install the dictionary database (opened directly by the app)
if [ "$DB_SOURCE" != "$DATA_DIR/$DB_NAME" ]; then
    install -Dm644 \
        "$DB_SOURCE" \
        "$DATA_DIR/$DB_NAME" || return 1
fi
[ -n "$DL_DIR" ] && rm -rf "$DL_DIR"

# 14b. Clean up files from older installs: the compressed archives and the
# extracted krumeto-<version>/ copies the app used to create.
rm -f "$DATA_DIR"/krumeto-*.db.zst
find "$DATA_DIR" -maxdepth 1 -type f -name 'krumeto-*.db' \
    ! -name "$DB_NAME" -delete 2>/dev/null
find "$DATA_DIR" -maxdepth 1 -type d \( -name 'krumeto-*' -o -name 'wn-*' \) \
    -exec rm -rf {} + 2>/dev/null

# 15. Install discovered application icon
install -Dm644 \
    "$ICON_SOURCE" \
    "$ICON_DIR/dev.silvanus.dictionary.svg" || return 1

# 15b. Install the tray icon, if present, under the name tray.c's
# StatusNotifierItem "IconName" property advertises. Most SNI hosts
# resolve IconName through the standard icon theme, and ~/.local/share
# is in XDG_DATA_DIRS for the desktop session (set again below for the
# launcher specifically), so this is enough without any IconThemePath
# workaround. Non-fatal if the tray asset isn't in this checkout.
for VARIANT in "dict-tray:dict-tray" "dict-tray-inactive:dict-tray-inactive"; do
    SRC_NAME="${VARIANT%%:*}"
    DST_NAME="${VARIANT##*:}"
    TRAY_ICON_SOURCE="$(find "$ROOT/icon/Tray" -type f -name "$SRC_NAME.svg" -print -quit 2>/dev/null)"
    if [ -n "$TRAY_ICON_SOURCE" ] && [ -s "$TRAY_ICON_SOURCE" ]; then
        install -Dm644 "$TRAY_ICON_SOURCE" "$ICON_DIR/$DST_NAME.svg" || return 1
        echo "Tray icon installed: $ICON_DIR/$DST_NAME.svg"
    else
        echo "WARNING: $SRC_NAME.svg not found; continuing (the app also ships its own copy)."
    fi
done

# 16. Create launcher
# XDG_DATA_DIRS is necessary because the application searches
# system data directories for dictionary/krumeto-<version>.db.

cat > "$BIN_DIR/dictionary-launcher" <<'LAUNCHER'
#!/bin/sh

export XDG_DATA_DIRS="${HOME}/.local/share:/usr/local/share:/usr/share"

exec "${HOME}/.local/bin/dictionary" "$@"
LAUNCHER

chmod 755 "$BIN_DIR/dictionary-launcher" || return 1

# 17. Escape desktop-entry paths
desktop_escape() {
    local VALUE="$1"
    VALUE="${VALUE//\\/\\\\}"
    VALUE="${VALUE// /\\s}"
    VALUE="${VALUE//%/%%}"
    printf '%s' "$VALUE"
}

LAUNCHER_PATH="$(desktop_escape "$BIN_DIR/dictionary-launcher")"

# 18. Create GNOME desktop entry
cat > "$APP_DIR/dev.silvanus.dictionary.desktop" <<EOF
[Desktop Entry]
Version=1.5
Type=Application
Name=Dictionary
GenericName=English Dictionary
Comment=Offline English dictionary powered by Open English WordNet
Exec=$LAUNCHER_PATH
TryExec=$LAUNCHER_PATH
Icon=dev.silvanus.dictionary
Terminal=false
Categories=Education;Utility;
StartupNotify=true
EOF

# 19. Validate desktop entry
if command -v desktop-file-validate >/dev/null 2>&1; then
    desktop-file-validate \
        "$APP_DIR/dev.silvanus.dictionary.desktop" || return 1
fi

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$APP_DIR"
fi

# 20. Verify installed files
for ITEM in \
    "$BIN_DIR/dictionary" \
    "$BIN_DIR/dictionary-launcher" \
    "$DATA_DIR/$DB_NAME" \
    "$ICON_DIR/dev.silvanus.dictionary.svg" \
    "$APP_DIR/dev.silvanus.dictionary.desktop"
do
    if [ ! -s "$ITEM" ]; then
        echo "ERROR: Installation verification failed: $ITEM"
        return 1
    fi
done

# 21. Check shared library dependencies
if command -v ldd >/dev/null 2>&1; then
    MISSING_LIBS="$(ldd "$BIN_DIR/dictionary" 2>/dev/null | grep 'not found' || true)"

    if [ -n "$MISSING_LIBS" ]; then
        echo "ERROR: Missing runtime libraries:"
        echo "$MISSING_LIBS"
        return 1
    fi
fi

# 22. Non-GUI application startup/version check
echo "Checking executable..."

if ! "$BIN_DIR/dictionary" --info >/dev/null 2>&1; then
    echo "WARNING: Application information check failed."
    echo "Inspect the executable and runtime dependencies."
fi

echo
echo "============================================"
echo "Dictionary installation completed."
echo "============================================"
echo "Binary:     $BIN_DIR/dictionary"
echo "Launcher:   $BIN_DIR/dictionary-launcher"
echo "Database:   $DATA_DIR/$DB_NAME"
echo "Icon:       $ICON_DIR/dev.silvanus.dictionary.svg"
echo "Desktop:    $APP_DIR/dev.silvanus.dictionary.desktop"
echo "Config:     $CONFIG_DIR"
echo
echo "Launch using:"
echo "$BIN_DIR/dictionary-launcher"
echo "============================================"

}

build_dictionary

```

---

## Project Structure after install

```

~/.local/
├── bin/
│   ├── dictionary
│   └── dictionary-launcher
│
└── share/
    ├── applications/
    │   └── dev.silvanus.dictionary.desktop
    │
    ├── dictionary/
    │   └── krumeto-<version>.db      (opened in place; no extracted copy)
    │
    └── icons/
        └── hicolor/
            └── scalable/
                └── apps/
                    ├── dev.silvanus.dictionary.svg
                    ├── dict-tray.svg
                    └── dict-tray-inactive.svg

~/.config/
└── dictionary/
    └── dictionary.json

```

---

## Configuration

Preferences (live search, double-click behavior, pronunciation accent, auto-paste on launch, dark mode, tray icon, background service, clipboard lookup and database location), search history and favorites are all managed from the in-app **Preferences** window (`Ctrl+P`) — no manual config editing required.

---

## Current Limitations

- Linux only, built against GTK4 and libadwaita
- English only, sourced from the Krumeto dictionary database
- One dictionary database at a time — you can choose which file in Preferences, but it must use Krumeto's schema

---

## License

This project's license has not been finalized yet — add a `LICENSE` file to the repository root and update this section (and the badge above) once chosen.

---

## About

The app's in-app **About** dialog (from the app menu, `F10`) links straight back here and to the [issue tracker](https://github.com/shlok2721-web/Dictionary/issues) — so feedback and bug reports are always one click away, no matter where someone got the build.

## Acknowledgments

- Krumeto — the dictionary database powering every definition. The compiled database is **CC BY-SA 4.0**; its upstream sources keep their own licenses (see the `license_notice` table inside the database: `LICENSE-DATA.md` and `THIRD_PARTY_NOTICES.txt`). Redistributing the database carries CC BY-SA attribution and share-alike obligations.
- [GTK](https://www.gtk.org/) & [libadwaita](https://gnome.pages.gitlab.gnome.org/libadwaita/) — the native GNOME application framework

<div align="center">
<br>
<sub>Dictionary. Every word, right where you work.</sub>
</div>
