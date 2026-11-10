# Kite Browser

A lightweight Linux browser built with GTK3 and WebKitGTK.

## Dependencies (Ubuntu / Debian)

```bash
sudo apt install build-essential cmake pkg-config libgtk-3-dev libwebkit2gtk-4.1-dev libsqlite3-dev python3
```

## Build

```bash
make          # optimized build -> ./mybrowser
make debug    # debug build -> ./mybrowser-debug
make test     # unit tests (URL handling, settings, history, session)
```

The build uses CMake and writes intermediates to `build/`. A direct CMake build works too:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

## Ad-block filters

Generate `filters.json` from the domain list:

```bash
python3 update_filters.py
# or
make filters
```

## Run

```bash
./mybrowser
# or
make run
```

`filters.json` is loaded from the same directory as the executable (or from the current working directory as a fallback).

## Features

- Tabbed browsing with incognito tabs that share one ephemeral session
- Permission prompts (camera/microphone, location, notifications, clipboard, and more) with optional per-site memory
- Friendly error pages for network failures, TLS certificate errors, and renderer crashes
- Persistent dark mode that recolors page colors while preserving hue instead of inverting them
- Find bar with match count and per-tab search state
- Omnibox normalization: `localhost`/IP addresses use `http://`, domains use `https://`, absolute paths open as `file://`, everything else searches DuckDuckGo
- Omnibox completion from history and bookmarks
- SQLite history and bookmarks with `kite://history` and `kite://bookmarks` pages
- Bookmark star in the toolbar and Ctrl+D; Ctrl+H opens history in a new tab
- Session restore on startup and recently closed tabs (Ctrl+Shift+T)
- Browser-style `kite://newtab` start page with search, shortcut tiles, recent history, and bookmarks (regular tabs only)
- Downloads manager with progress, cancel, open, and clear-finished actions
- Per-site zoom that survives reloads and restarts
- Ad-blocking via WebKit content blockers

## Settings

Settings and remembered permission decisions are stored in `~/.config/kite/settings.ini`:

```ini
[appearance]
dark_mode=false

[general]
homepage=kite://newtab

[session]
restore=true

[downloads]
directory=/home/user/Downloads
ask=false

[permissions]
media|example.com=allow

[zoom]
zoom|example.com=1.2
```

- `homepage` is used by the Home button (default `kite://newtab`).
- `restore` restores the previous session on startup alongside a fresh start page, which opens first and stays active.
- `directory` is the default download folder; `ask=true` shows a file chooser for every download.
- `[zoom]` stores per-host zoom levels, written automatically by Ctrl++ / Ctrl+-.
- `[permissions]` is written from the permission prompts; delete keys to ask again.

History and bookmarks live in `~/.local/share/kite/kite.db` (SQLite). The current
session is saved to `~/.config/kite/session.ini` (mode `0600`) and is only
restored when session restore is enabled. Incognito tabs do not write history,
bookmarks, session state, permission decisions, or per-site zoom, and they do not
see your regular history or bookmarks.

## Shortcuts

| Shortcut | Action |
|----------|--------|
| Ctrl+T | New tab |
| Ctrl+W | Close tab |
| Ctrl+Tab / Ctrl+Shift+Tab | Next / previous tab |
| Ctrl+Shift+T | Reopen most recently closed tab |
| Ctrl+L | Focus URL bar |
| Ctrl+R | Reload |
| Reload button while loading | Stop loading |
| Ctrl+F | Find in page |
| Ctrl+D | Bookmark / unbookmark the current page |
| Ctrl+H | History page |
| Ctrl+Shift+O | Bookmarks page |
| Ctrl+Shift+P | Incognito tab |
| F9 / Ctrl+Shift+D | Toggle dark mode |
| F12 / Ctrl+Shift+I | DevTools |
| Ctrl++ / Ctrl+- / Ctrl+0 | Zoom in / out / reset (per site) |

Omnibox bangs: `!yt`, `!gh`, `!w`, `!r` (YouTube, GitHub, Wikipedia, Reddit).

Internal pages: `kite://newtab`, `kite://history`, and `kite://bookmarks` can be
typed in the address bar at any time.

## Layout

```
src/main.cpp             startup, context and global init
src/browser_window.*     window, toolbar, shortcuts, omnibox, session save/restore
src/tabs.*               tab lifecycle, history/zoom hooks, recently closed tabs
src/history.*            SQLite history/bookmarks schema and queries
src/session.*            session.ini load/save/clear
src/settings.*           settings.ini, permission memory, per-site zoom
src/kite_pages.*         kite:// internal pages
src/downloads.*          downloads popover and destination handling
src/adblock.*            WebKit content blocker
src/dark_mode.*          dark-mode CSS injection and toolbar toggle
src/errors.*             error pages and TLS/crash handlers
src/find_bar.*           find-in-page UI
src/permissions.*        permission prompts and persistence
src/url_utils.*          URL normalization, bangs, host extraction
tests/                   unit tests for UI-independent logic
Makefile                 convenience wrapper around CMake
```
