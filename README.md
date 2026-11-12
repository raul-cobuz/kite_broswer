# Kite Browser

A lightweight Linux browser built with GTK3 and WebKitGTK.

## Dependencies (Ubuntu / Debian)

```bash
sudo apt install build-essential cmake pkg-config libgtk-3-dev libwebkit2gtk-4.1-dev libjavascriptcoregtk-4.1-dev libsqlite3-dev python3
```

## Build

```bash
make          # optimized build -> ./mybrowser
make debug    # debug build -> ./mybrowser-debug
make test     # unit tests for settings, history, session, profiles, sync, and more
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

To update the desktop copy launched by the Kite icon:

```bash
make install
```

This installs the current build as `~/.local/bin/kite` together with `filters.json`
(override with `BINDIR=/some/path`). It also installs `kite.desktop` and the Kite
icon under `~/.local/share`, so Kite appears in the desktop application menu and
can be selected as the default browser. Restart a running Kite window to pick up
the new binary.

To register Kite as the default web browser from a terminal:

```bash
make default-browser
# or ask the app from kite://settings → Default browser
```

`make default-browser` uses `xdg-settings set default-web-browser kite.desktop`.
Some desktops still show a confirmation prompt, and a few (for example GNOME
without the relevant portal) only allow changing the default from their own
settings panel.

## Features

- Tabbed browsing with incognito tabs that share one ephemeral session
- Split view: two tabs side by side in one window, with the address bar, navigation, and find bar following the focused pane
- Tab context menu with Duplicate, Copy URL, Mute, Pin, Close other tabs, Close tabs to the right, and Move to split pane
- Pinned tabs shrink to their favicon and stay at the left of the tab strip; pinned state is saved in the session
- Command palette on Ctrl+K or Ctrl+Shift+A that searches open tabs, history, bookmarks, and actions such as “New incognito tab” or “Toggle dark mode”
- Screenshots (visible area or the full page) to PNG and native printing through WebKit's GTK print dialog
- Named profiles at `kite://profiles`: avatar initials/colours, isolated history, bookmarks, settings, extensions, and website data; switching relaunches Kite
- Guest profile at `kite://profiles`: a throwaway profile whose history, cookies, and settings are deleted when Kite closes
- Customizable start page: background image, clock, Open-Meteo weather widget, up to twelve custom quick links, and a compact/expanded layout
- Folder-based sync at `kite://sync` that merges bookmarks, pinned links, history, and optional settings through `kite-sync.json`
- Omnibox suggestions from history, bookmarks, pins, open tabs, and Kite pages, with engine-aware search
- Search settings at `kite://settings` for Google, DuckDuckGo, Startpage, or Brave, plus an automatic Google verification fallback
- Permission prompts (camera/microphone, location, notifications, clipboard, and more) with optional per-site memory
- Friendly error pages for network failures, TLS certificate errors, and renderer crashes
- Persistent dark mode that recolors page colors while preserving hue instead of inverting them, with a Google Search fallback that handles gradient result cards
- Find bar with match count and per-tab search state
- Omnibox normalization: `localhost`/IP addresses use `http://`, domains use `https://`, absolute paths open as `file://`, everything else searches Google
- Omnibox completion from history and bookmarks
- SQLite history, bookmarks, and pinned start-page links with `kite://history` and `kite://bookmarks` pages
- Bookmark star in the toolbar and Ctrl+D; Ctrl+H opens history in a new tab
- Session restore on startup and recently closed tabs (Ctrl+Shift+T)
- Browser-style `kite://newtab` start page with search, a recent-searches strip, up to five pinned links, recent history, and bookmarks (regular tabs only)
- Extension manager at `kite://extensions` with folder installation, enable/disable, reload, removal, content scripts/styles, background scripts or pages, and practical `chrome.*` APIs
- Downloads manager with progress, cancel, open, and clear-finished actions; every download asks where to save
- Per-site zoom that survives reloads and restarts
- Ad-blocking via WebKit content blockers, plus in-page removal of YouTube player/feed ads and ad-block detection prompts

## Settings

Settings and remembered permission decisions are stored in `~/.config/kite/settings.ini`:

```ini
[appearance]
dark_mode=false

[general]
homepage=kite://newtab

[search]
engine=google
google_fallback=true

[session]
restore=true

[downloads]
directory=/home/user/Downloads

[sync]
folder=/home/user/KiteSync
on_startup=false
include_settings=false

[permissions]
media|example.com=allow

[zoom]
zoom|example.com=1.2

[newtab]
clock=true
weather=false
weather_city=Lisbon
weather_units=celsius
background=
compact=false
link_count=1
link0_title=Docs
link0_url=https://example.com/docs
```

- `homepage` is used by the Home button (default `kite://newtab`).
- `engine` is `google` (default), `duckduckgo`, `startpage`, or `brave`.
- `google_fallback` rewrites Google `/sorry/` verification pages to DuckDuckGo results instead of showing the robot check.
- `restore` restores the previous session on startup alongside a fresh start page, which opens after any pinned restored tabs and stays active.
- `directory` is the folder the save dialog starts in; every download shows a file chooser.
- `[sync]` controls the shared folder, startup syncing, and whether settings are merged along with bookmarks, pins, and history.
- `[zoom]` stores per-host zoom levels, written automatically by Ctrl++ / Ctrl+-.
- `[newtab]` stores start-page customization: background image URL/path, clock and weather toggles, weather city/units, compact layout, and up to twelve custom quick links. These are separate from the pinned links on the start page.
- `[permissions]` is written from the permission prompts; delete keys to ask again.

History, bookmarks, and pinned start-page links live in
`~/.local/share/kite/kite.db` (SQLite). The current session is saved to
`~/.config/kite/session.ini` (mode `0600`) and is only restored when session
restore is enabled. Incognito tabs do not write history, bookmarks, pinned links,
session state, permission decisions, or per-site zoom, and they do not see your
regular history or bookmarks.

## Profiles and sync

Profiles are managed from `kite://profiles`. The default profile keeps the legacy
paths below; named profiles use `~/.config/kite/profiles/<id>` and
`~/.local/share/kite/profiles/<id>`. Switching writes an active-profile marker and
relaunches Kite. Start directly in a profile with:

```bash
kite --profile=<id>
# browse with a throwaway guest profile
kite --guest
# or open a URL/query directly
kite --url=kite://settings
```

Guest mode creates a temporary data directory, skips session restore, and removes
the directory when Kite exits. Named profiles and guest profiles share the same
window but never share cookies, history, bookmarks, or settings.

Sync uses a folder you choose on `kite://sync`. Kite writes `kite-sync.json` there
and merges bookmarks, pinned links, and history, optionally including homepage,
search engine, Google fallback, dark mode, session restore, ad blocking,
tracking-parameter stripping, and clear-on-exit. Cookies, passwords, and extensions
stay local.

## Custom extensions

Kite loads extensions from `~/.local/share/kite/extensions/<id>/manifest.json`.
Use the Extensions button in the toolbar (or type `kite://extensions`) to install
a folder, enable or disable an extension, reload all extensions, or remove one.
The folder must contain a `manifest.json`; the whole folder is copied into the
Kite data directory.

Kite supports WebExtension-style content scripts and styles plus a practical
subset of the extension runtime. A manifest can declare `content_scripts` entries
with `matches`, `js`, `css`, `run_at`, and `all_frames`, and a `background` entry
with `scripts`, `page`, or `service_worker` (loaded as a background script).
Supported APIs include `chrome.storage`, `chrome.runtime` messaging,
`chrome.tabs`, `chrome.notifications`, and `chrome.permissions`, backed by
`kite-ext://` pages. Scripts can also use standard DOM APIs.

This is not a full Chrome extension implementation. WebKitGTK cannot install
Chrome Web Store `.crx` packages or provide Chrome's `webRequest`,
declarativeNetRequest, or service-worker execution model. Background pages are
hosted in offscreen windows, and extension content scripts share Kite's content
manager (so they also run in incognito tabs).

```json
{
  "manifest_version": 3,
  "name": "My extension",
  "version": "1.0",
  "description": "What this extension does",
  "content_scripts": [
    {
      "matches": ["https://example.com/*"],
      "js": ["content.js"],
      "css": ["style.css"],
      "run_at": "document_end"
    }
  ]
}
```

Content scripts never run on `kite://` pages. `file://` pages are only matched
when a content script explicitly includes a `file://` pattern; broad patterns
such as `<all_urls>` expand to `http://` and `https://` only.

## Shortcuts

| Shortcut | Action |
|----------|--------|
| Ctrl+T | New tab |
| Ctrl+W | Close tab |
| Ctrl+Tab / Ctrl+Shift+Tab | Next / previous tab |
| Ctrl+\ | Toggle side-by-side split view |
| Ctrl+Shift+T | Reopen most recently closed tab |
| Ctrl+L | Focus URL bar |
| Ctrl+R | Reload |
| Reload button while loading | Stop loading |
| Ctrl+F | Find in page |
| Ctrl+D | Bookmark / unbookmark the current page |
| Ctrl+H | History page |
| Ctrl+Shift+O | Bookmarks page |
| Ctrl+Shift+P | Incognito tab |
| Ctrl+K / Ctrl+Shift+A | Search tabs, history, bookmarks, and commands |
| Ctrl+P | Print the current page |
| Ctrl+Shift+S | Save a screenshot of the visible page |
| Ctrl+Shift+E | Save a full-page screenshot |
| F9 / Ctrl+Shift+D | Toggle dark mode |
| F12 / Ctrl+Shift+I | DevTools |
| Ctrl++ / Ctrl+- / Ctrl+0 | Zoom in / out / reset (per site) |

Omnibox bangs: `!yt`, `!gh`, `!w`, `!r` (YouTube, GitHub, Wikipedia, Reddit).

Internal pages: `kite://newtab`, `kite://history`, `kite://bookmarks`,
`kite://settings`, `kite://profiles`, `kite://sync`, and `kite://extensions` can be
typed in the address bar at any time.

## Layout

```
src/main.cpp             startup, context and global init
src/browser_window.*     window, toolbar, shortcuts, omnibox, session save/restore
src/tabs.*               tab lifecycle, pinned tabs, context menu, history/zoom hooks
src/command_palette.*    Ctrl+K search across tabs, history, bookmarks, and actions
src/page_tools.*         screenshot capture and WebKit print dialog
src/history.*            SQLite history/bookmarks/pinned-link schema and queries
src/session.*            session.ini load/save/clear, including pinned tabs
src/settings.*           settings.ini, permission memory, per-site zoom, start page
src/kite_pages.*         kite:// internal pages
src/profile.*            profile registry, guest profile, avatars, active-profile switching
src/sync.*               folder-based sync of bookmarks, pins, history, and settings
src/omnibox.*            local-first address-bar suggestions
src/app_context.*        persistent WebKit context and cookie storage
src/extensions.*         extension manifests, content scripts, background pages, chrome.* APIs
src/downloads.*          downloads popover and destination handling
src/adblock.*            WebKit content blocker
src/dark_mode.*          dark-mode CSS injection and toolbar toggle
src/errors.*             error pages and TLS/crash handlers
src/find_bar.*           find-in-page UI
src/permissions.*        permission prompts and persistence
src/url_utils.*          URL normalization, bangs, host extraction
tests/                   unit tests for UI-independent logic
data/kite.desktop.in     desktop entry installed by make install
data/kite.svg            scalable application icon
Makefile                 convenience wrapper around CMake
```
