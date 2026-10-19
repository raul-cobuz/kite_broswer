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
