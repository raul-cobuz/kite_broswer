# Kite Browser

A lightweight Linux browser built with GTK3 and WebKitGTK.

## Dependencies (Ubuntu / Debian)

```bash
sudo apt install build-essential pkg-config libgtk-3-dev libwebkit2gtk-4.1-dev python3
```

## Build

```bash
make
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
- Persistent dark mode
- Find bar with match count and per-tab search state
- Omnibox normalization: `localhost`/IP addresses use `http://`, domains use `https://`, absolute paths open as `file://`, everything else searches DuckDuckGo
- Ad-blocking via WebKit content blockers

## Settings

Settings and remembered permission decisions are stored in `~/.config/kite/settings.ini`.

## Shortcuts

| Shortcut | Action |
|----------|--------|
| Ctrl+T | New tab |
| Ctrl+W | Close tab |
| Ctrl+Tab / Ctrl+Shift+Tab | Next / previous tab |
| Ctrl+L | Focus URL bar |
| Ctrl+R | Reload |
| Reload button while loading | Stop loading |
| Ctrl+F | Find in page |
| Ctrl+Shift+P | Incognito tab |
| F9 / Ctrl+Shift+D | Toggle dark mode |
| F12 / Ctrl+Shift+I | DevTools |
| Ctrl++ / Ctrl+- / Ctrl+0 | Zoom |

Omnibox bangs: `!yt`, `!gh`, `!w`, `!r` (YouTube, GitHub, Wikipedia, Reddit).
