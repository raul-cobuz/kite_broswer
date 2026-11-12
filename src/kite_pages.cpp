#include "kite_pages.h"

#include <glib.h>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

#include "browser_window.h"
#include "extensions.h"
#include "history.h"
#include "profile.h"
#include "settings.h"
#include "sync.h"
#include "url_utils.h"

static std::string html_escape(const std::string &text) {
    gchar *escaped = g_markup_escape_text(text.c_str(), (gssize)text.size());
    std::string result = escaped ? escaped : "";
    g_free(escaped);
    return result;
}

static std::string format_visit_time(gint64 micros) {
    if (micros <= 0)
        return "";
    GDateTime *dt = g_date_time_new_from_unix_local(micros / G_USEC_PER_SEC);
    if (!dt)
        return "";
    gchar *formatted = g_date_time_format(dt, "%b %e, %H:%M");
    std::string result = formatted ? formatted : "";
    g_free(formatted);
    g_date_time_unref(dt);
    return result;
}

static std::string query_param(const gchar *query, const gchar *name) {
    if (!query || !*query)
        return "";
    GHashTable *params = g_uri_parse_params(query, -1, "&", G_URI_PARAMS_WWW_FORM, nullptr);
    if (!params)
        return "";
    const gchar *value = (const gchar *)g_hash_table_lookup(params, name);
    std::string result = value ? value : "";
    g_hash_table_destroy(params);
    return result;
}

static std::string escape_query_value(const std::string &value) {
    gchar *escaped = g_uri_escape_string(value.c_str(), nullptr, FALSE);
    std::string result = escaped ? escaped : "";
    g_free(escaped);
    return result;
}

struct SearchHit {
    std::string query;
    std::string url;
    std::string engine;
};

static bool search_hit_from_entry(const HistoryEntry &entry, SearchHit &hit) {
    GError *error = nullptr;
    GUri *parsed = g_uri_parse(entry.url.c_str(), G_URI_FLAGS_NONE, &error);
    if (!parsed) {
        g_clear_error(&error);
        return false;
    }

    const gchar *host_value = g_uri_get_host(parsed);
    const gchar *path_value = g_uri_get_path(parsed);
    const gchar *query_value = g_uri_get_query(parsed);

    std::string host = host_value ? host_value : "";
    std::string path = path_value ? path_value : "";
    std::string query = query_value ? query_value : "";
    g_uri_unref(parsed);

    if (host.rfind("www.", 0) == 0)
        host = host.substr(4);
    if (host.empty())
        return false;

    std::string search_query;
    std::string engine;
    bool is_google = host == "google.com" || host.rfind("google.", 0) == 0;
    if (is_google && path.rfind("/search", 0) == 0) {
        search_query = query_param(query.c_str(), "q");
        engine = "Google";
    } else if ((host == "youtube.com" || host == "m.youtube.com") && path.rfind("/results", 0) == 0) {
        search_query = query_param(query.c_str(), "search_query");
        engine = "YouTube";
    } else if (host == "github.com" && path.rfind("/search", 0) == 0) {
        search_query = query_param(query.c_str(), "q");
        engine = "GitHub";
    } else if ((host == "wikipedia.org" || g_str_has_suffix(host.c_str(), ".wikipedia.org")) &&
               (path.rfind("/wiki/Special:Search", 0) == 0 || path.rfind("/w/index.php", 0) == 0)) {
        search_query = query_param(query.c_str(), "search");
        engine = "Wikipedia";
    } else if ((host == "reddit.com" || g_str_has_suffix(host.c_str(), ".reddit.com")) &&
               path.rfind("/search", 0) == 0) {
        search_query = query_param(query.c_str(), "q");
        engine = "Reddit";
    } else if ((host == "duckduckgo.com" || host == "html.duckduckgo.com" || host == "lite.duckduckgo.com") &&
               (path == "/" || path == "/html" || path == "/html/" || path == "/lite" || path == "/lite/")) {
        search_query = query_param(query.c_str(), "q");
        engine = "DuckDuckGo";
    } else if (host == "startpage.com" && path.rfind("/sp/search", 0) == 0) {
        search_query = query_param(query.c_str(), "query");
        engine = "Startpage";
    } else if (host == "search.brave.com" && path.rfind("/search", 0) == 0) {
        search_query = query_param(query.c_str(), "q");
        engine = "Brave";
    } else if ((host == "bing.com" || host.rfind("bing.", 0) == 0) && path.rfind("/search", 0) == 0) {
        search_query = query_param(query.c_str(), "q");
        engine = "Bing";
    }

    if (search_query.empty() || engine.empty())
        return false;

    hit.query = search_query;
    hit.url = entry.url;
    hit.engine = engine;
    return true;
}

static std::vector<SearchHit> recent_search_hits(int limit) {
    std::vector<SearchHit> hits;
    std::unordered_set<std::string> seen;
    for (const auto &entry : history_recent(200)) {
        SearchHit hit;
        if (!search_hit_from_entry(entry, hit))
            continue;

        gchar *folded = g_utf8_casefold(hit.query.c_str(), -1);
        std::string key = folded ? folded : hit.query;
        g_free(folded);
        if (!seen.insert(key).second)
            continue;

        hits.push_back(hit);
        if ((int)hits.size() >= limit)
            break;
    }
    return hits;
}

static std::string remove_href(const char *page, const std::string &url) {
    gchar *escaped = g_uri_escape_string(url.c_str(), nullptr, TRUE);
    std::string href = std::string("kite://") + page + "/remove?url=" + (escaped ? escaped : "");
    g_free(escaped);
    return href;
}

static std::string host_label(const std::string &url, const std::string &host) {
    if (!host.empty())
        return host;
    return url_host(url);
}

static std::string display_host(const std::string &host) {
    if (host.rfind("www.", 0) == 0)
        return host.substr(4);
    return host;
}

static char host_initial(const std::string &host) {
    std::string plain = display_host(host);
    for (char c : plain) {
        if (g_ascii_isalnum(c))
            return (char)g_ascii_toupper(c);
    }
    return '?';
}

static int host_hue(const std::string &host) {
    guint hash = g_str_hash(host.c_str());
    return (int)(hash % 360);
}

static std::string tile_style(const std::string &host, bool dark) {
    int hue = host_hue(host);
    if (dark)
        return "--tile-bg:hsl(" + std::to_string(hue) + ",30%,26%);--tile-fg:hsl(" + std::to_string(hue) +
               ",75%,78%)";
    return "--tile-bg:hsl(" + std::to_string(hue) + ",72%,92%);--tile-fg:hsl(" + std::to_string(hue) +
           ",62%,32%)";
}

static std::string favicon(const std::string &host, bool dark) {
    if (host.empty())
        return "<span class=\"fav\">?</span>";
    std::string letter(1, host_initial(host));
    return "<span class=\"fav\" style=\"" + tile_style(host, dark) + "\">" + html_escape(letter) + "</span>";
}

static const char *KITE_MARK_SVG =
    "<svg class=\"kite-mark\" viewBox=\"0 0 64 64\" aria-hidden=\"true\">"
    "<path d=\"M32 3 55 25 32 61 9 25Z\" fill=\"var(--accent)\"/>"
    "<path d=\"M32 3 55 25 32 61Z\" fill=\"var(--accent-strong)\" opacity=\".82\"/>"
    "<path d=\"M32 3v58M9 25h46\" fill=\"none\" stroke=\"rgba(255,255,255,.42)\" stroke-width=\"1.6\"/>"
    "</svg>";

static const char *CLOCK_SVG =
    "<svg viewBox=\"0 0 24 24\" aria-hidden=\"true\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"1.9\" "
    "stroke-linecap=\"round\" stroke-linejoin=\"round\"><circle cx=\"12\" cy=\"12\" r=\"8.5\"/>"
    "<path d=\"M12 7.5V12l3 2\"/></svg>";

static const char *PIN_SVG =
    "<svg viewBox=\"0 0 24 24\" aria-hidden=\"true\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" "
    "stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"M9 3.5h6l-1 6 3.5 3.5H6.5L10 9.5Z\"/>"
    "<path d=\"M12 13v7\"/></svg>";

static const char *SEARCH_SVG =
    "<svg class=\"search-icon\" viewBox=\"0 0 24 24\" aria-hidden=\"true\" fill=\"none\" stroke=\"currentColor\" "
    "stroke-width=\"2\" stroke-linecap=\"round\"><circle cx=\"11\" cy=\"11\" r=\"6.5\"/><path d=\"m16 16 4.5 "
    "4.5\"/></svg>";

static const char *INCOGNITO_SVG =
    "<svg class=\"incognito-icon\" viewBox=\"0 0 120 80\" aria-hidden=\"true\">"
    "<path d=\"M20 43c10-2 18-7 22-16h36c4 9 12 14 22 16-3 10-12 15-27 15H47c-15 0-24-5-27-15Z\" "
    "fill=\"currentColor\"/>"
    "<circle cx=\"41\" cy=\"47\" r=\"8\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"4\"/>"
    "<circle cx=\"79\" cy=\"47\" r=\"8\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"4\"/>"
    "<path d=\"M49 47h22\" stroke=\"currentColor\" stroke-width=\"4\" stroke-linecap=\"round\"/>"
    "</svg>";

static const char *PAGE_CSS = R"CSS(
:root {
  color-scheme: light;
  --bg: #f8f9fa;
  --bg-soft: #ffffff;
  --bg-muted: #f1f3f4;
  --text: #202124;
  --muted: #5f6368;
  --border: #dadce0;
  --accent: #1a73e8;
  --accent-strong: #1557b0;
  --accent-soft: rgba(26, 115, 232, .10);
  --danger: #c5221f;
  --glow: rgba(66, 133, 244, .12);
  --shadow: 0 1px 2px rgba(60,64,67,.10), 0 2px 8px rgba(60,64,67,.08);
  --tile-shadow: 0 1px 2px rgba(60,64,67,.08);
}
html[data-theme="dark"] {
  color-scheme: dark;
  --bg: #17181b;
  --bg-soft: #202124;
  --bg-muted: #2a2b2f;
  --text: #e8eaed;
  --muted: #9aa0a6;
  --border: #3c4043;
  --accent: #8ab4f8;
  --accent-strong: #aecbfa;
  --accent-soft: rgba(138, 180, 248, .14);
  --danger: #f28b82;
  --glow: rgba(138, 180, 248, .10);
  --shadow: 0 1px 2px rgba(0,0,0,.45);
  --tile-shadow: 0 1px 2px rgba(0,0,0,.40);
}
* { box-sizing: border-box; }
html, body { min-height: 100%; }
body {
  margin: 0;
  background: var(--bg);
  color: var(--text);
  font-family: system-ui, -apple-system, "Segoe UI", Roboto, Ubuntu, Cantarell, sans-serif;
  font-size: 15px;
  line-height: 1.5;
  -webkit-font-smoothing: antialiased;
}
a { color: var(--accent); text-decoration: none; }
a:hover { text-decoration: underline; }
::selection { background: var(--accent-soft); }
.topbar {
  width: 100%;
  max-width: 1180px;
  margin: 0 auto;
  padding: 18px 24px;
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 18px;
}
.brand {
  display: inline-flex;
  align-items: center;
  gap: 10px;
  color: var(--text);
  font-weight: 600;
  font-size: 16px;
  letter-spacing: -.02em;
}
.brand:hover { text-decoration: none; }
.kite-mark { width: 56px; height: 56px; flex: none; }
.brand .kite-mark { width: 22px; height: 22px; }
.topnav { display: flex; align-items: center; gap: 4px; }
.topnav a {
  color: var(--muted);
  font-size: 13px;
  padding: 8px 12px;
  border-radius: 999px;
}
.topnav a:hover { color: var(--text); background: var(--bg-muted); text-decoration: none; }
.badge {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  padding: 5px 12px;
  border-radius: 999px;
  background: var(--accent-soft);
  color: var(--accent);
  font-size: 12px;
  font-weight: 600;
  letter-spacing: .02em;
}
body.newtab-page {
  min-height: 100vh;
  display: flex;
  flex-direction: column;
  background:
    radial-gradient(960px 440px at 50% -160px, var(--glow), transparent 72%),
    var(--bg);
  background-attachment: fixed;
}
body.newtab-page.has-shelf .shelf { margin-top: 0; }
.hero.compact { flex: none !important; justify-content: flex-start !important; padding-top: clamp(28px, 7vh, 64px) !important; padding-bottom: 38px !important; }
body.incognito-page {
  --bg: #121214;
  --bg-soft: #1d1b22;
  --bg-muted: #2a2731;
  --text: #ece8f3;
  --muted: #a79fb3;
  --border: #3b3546;
  --accent: #c58af9;
  --accent-strong: #d7b3ff;
  --accent-soft: rgba(197, 138, 249, .14);
  --danger: #f28b82;
  --glow: rgba(142, 68, 173, .22);
  --shadow: 0 1px 2px rgba(0,0,0,.45);
  --tile-shadow: 0 1px 2px rgba(0,0,0,.4);
}
.hero {
  flex: 1 1 auto;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  width: 100%;
  max-width: 780px;
  margin: 0 auto;
  padding: 42px 24px 52px;
  text-align: center;
}
.hero .kite-mark { width: 74px; height: 74px; margin-bottom: 10px; }
.incognito-icon {
  width: 112px;
  height: 76px;
  color: var(--accent);
  margin-bottom: 16px;
}
.wordmark {
  margin: 0;
  font-size: clamp(34px, 5vw, 48px);
  font-weight: 600;
  letter-spacing: -.045em;
  line-height: 1.1;
}
.tagline {
  margin: 10px 0 30px;
  color: var(--muted);
  font-size: 16px;
  max-width: 540px;
}
.search {
  display: flex;
  align-items: center;
  gap: 12px;
  width: 100%;
  max-width: 640px;
  height: 54px;
  padding: 0 20px;
  background: var(--bg-soft);
  border: 1px solid var(--border);
  border-radius: 999px;
  box-shadow: var(--shadow);
  transition: border-color .15s ease, box-shadow .15s ease;
  text-align: left;
}
.search:focus-within {
  border-color: var(--accent);
  box-shadow: 0 0 0 4px var(--accent-soft), var(--shadow);
}
.search-icon { width: 20px; height: 20px; flex: none; color: var(--muted); }
.search input {
  flex: 1;
  min-width: 0;
  height: 100%;
  padding: 0;
  border: 0;
  outline: 0;
  background: transparent;
  color: var(--text);
  font: inherit;
  font-size: 16px;
}
.search input::placeholder { color: var(--muted); opacity: .9; }
.search button { display: none; }
.shortcuts {
  width: 100%;
  max-width: 720px;
  margin: 34px auto 0;
  display: flex;
  flex-wrap: wrap;
  justify-content: center;
  gap: 16px 8px;
}
.shortcut {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 8px;
  width: 82px;
  padding: 6px 4px;
  border-radius: 12px;
  color: var(--text);
  transition: background-color .15s ease, transform .15s ease;
}
.shortcut:hover { background: var(--accent-soft); text-decoration: none; transform: translateY(-1px); }
.shortcut .tile {
  width: 54px;
  height: 54px;
  border-radius: 16px;
  display: flex;
  align-items: center;
  justify-content: center;
  background: var(--tile-bg, var(--bg-soft));
  color: var(--tile-fg, var(--accent));
  border: 1px solid rgba(127, 127, 127, .14);
  box-shadow: var(--tile-shadow);
  font-size: 22px;
  font-weight: 600;
}
.shortcut .tile.fixed { background: var(--accent-soft); color: var(--accent); border-color: transparent; }
.shortcut .tile svg { width: 26px; height: 26px; }
.shortcut .name {
  max-width: 100%;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  font-size: 12px;
  color: var(--muted);
}
.shortcut:hover .name { color: var(--text); }

/* Recent searches strip */
.recent-searches {
  display: flex;
  align-items: center;
  gap: 12px;
  width: 100%;
  max-width: 720px;
  margin: 20px auto 0;
  padding: 7px 10px 7px 14px;
  background: var(--bg-soft);
  border: 1px solid var(--border);
  border-radius: 16px;
  box-shadow: var(--tile-shadow);
  text-align: left;
  overflow: hidden;
}
.rs-label {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  flex: none;
  color: var(--muted);
  font-size: 11px;
  font-weight: 700;
  letter-spacing: .07em;
  text-transform: uppercase;
  white-space: nowrap;
}
.rs-label svg { width: 14px; height: 14px; }
.rs-chips {
  display: flex;
  align-items: center;
  gap: 7px;
  overflow-x: auto;
  padding: 2px;
  scrollbar-width: none;
}
.rs-chips::-webkit-scrollbar { display: none; }
.search-chip {
  display: inline-flex;
  align-items: center;
  gap: 7px;
  padding: 7px 12px;
  border-radius: 999px;
  background: var(--bg-muted);
  border: 1px solid transparent;
  color: var(--text);
  font-size: 12.5px;
  white-space: nowrap;
  transition: background-color .15s ease, border-color .15s ease, color .15s ease, transform .15s ease;
}
.search-chip:hover {
  background: var(--accent-soft);
  border-color: var(--accent);
  color: var(--accent);
  text-decoration: none;
  transform: translateY(-1px);
}
.search-chip .q { max-width: 230px; overflow: hidden; text-overflow: ellipsis; }
.search-chip .go { color: var(--muted); font-size: 12px; }
.search-chip:hover .go { color: var(--accent); }

/* Quick links: pinned and recent */
.quicklinks {
  width: 100%;
  max-width: 820px;
  margin: 30px auto 0;
  text-align: left;
}
.quicklinks + .quicklinks { margin-top: 22px; }
.quicklinks-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  min-height: 26px;
  padding: 0 6px;
  margin-bottom: 8px;
}
.quicklinks-head h2 {
  margin: 0;
  color: var(--muted);
  font-size: 11.5px;
  font-weight: 700;
  letter-spacing: .09em;
  text-transform: uppercase;
}
.quicklinks-links { display: flex; align-items: center; gap: 14px; font-size: 12px; }
.quicklinks-note { color: var(--muted); font-size: 11.5px; }
.quicklinks-empty {
  margin: 0;
  padding: 14px 16px;
  border: 1px dashed var(--border);
  border-radius: 14px;
  color: var(--muted);
  font-size: 13px;
}
.quicklinks .shortcuts {
  width: 100%;
  max-width: none;
  margin: 0;
  justify-content: flex-start;
  gap: 12px 6px;
}
.pinned-shortcuts .pin-card { position: relative; width: 82px; }
.pin-card .shortcut { width: 100%; }
.pin-card .tile { position: relative; }
.pin-flag {
  position: absolute;
  right: -4px;
  bottom: -4px;
  width: 18px;
  height: 18px;
  display: flex;
  align-items: center;
  justify-content: center;
  border-radius: 50%;
  border: 1px solid var(--border);
  background: var(--bg-soft);
  color: var(--accent);
}
.pin-flag svg { width: 10px; height: 10px; }
.pin-remove {
  position: absolute;
  top: -7px;
  right: 0;
  z-index: 2;
  width: 22px;
  height: 22px;
  display: flex;
  align-items: center;
  justify-content: center;
  border-radius: 50%;
  border: 1px solid var(--border);
  background: var(--bg-soft);
  color: var(--muted);
  font-size: 15px;
  line-height: 1;
  opacity: 0;
  transform: scale(.85);
  transition: opacity .15s ease, transform .15s ease, color .15s ease, border-color .15s ease;
}
.pin-card:hover .pin-remove,
.pin-card:focus-within .pin-remove { opacity: 1; transform: none; }
.pin-remove:hover { color: var(--danger); border-color: var(--danger); text-decoration: none; }
.pin-add { position: relative; }
.pin-add summary {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  padding: 6px 12px;
  border: 1px solid var(--border);
  border-radius: 999px;
  background: var(--bg-soft);
  color: var(--muted);
  font-size: 12px;
  cursor: pointer;
  list-style: none;
  transition: background-color .15s ease, border-color .15s ease, color .15s ease;
}
.pin-add summary::-webkit-details-marker { display: none; }
.pin-add summary:hover,
.pin-add[open] summary { color: var(--accent); background: var(--accent-soft); border-color: var(--accent); }
.pin-add summary .plus { font-size: 15px; line-height: 1; }
.pin-form {
  position: absolute;
  right: 0;
  top: calc(100% + 8px);
  z-index: 20;
  display: grid;
  grid-template-columns: minmax(110px, .9fr) minmax(160px, 1.3fr) auto;
  gap: 8px;
  width: min(500px, calc(100vw - 48px));
  padding: 12px;
  border: 1px solid var(--border);
  border-radius: 14px;
  background: var(--bg-soft);
  box-shadow: 0 10px 30px rgba(0, 0, 0, .16);
}
.pin-form input {
  min-width: 0;
  padding: 9px 12px;
  border: 1px solid var(--border);
  border-radius: 10px;
  outline: 0;
  background: var(--bg);
  color: var(--text);
  font: inherit;
  font-size: 13px;
}
.pin-form input:focus { border-color: var(--accent); box-shadow: 0 0 0 3px var(--accent-soft); }
.btn {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  gap: 6px;
  padding: 8px 14px;
  border: 1px solid var(--border);
  border-radius: 10px;
  background: var(--bg-soft);
  color: var(--text);
  font-size: 12.5px;
  font-weight: 600;
  cursor: pointer;
  transition: background-color .15s ease, border-color .15s ease, color .15s ease;
}
.btn:hover { background: var(--bg-muted); text-decoration: none; }
.btn.primary { border-color: var(--accent); background: var(--accent); color: #fff; }
html[data-theme="dark"] .btn.primary { color: #121214; }
.btn.primary:hover { background: var(--accent-strong); border-color: var(--accent-strong); }
.btn.danger { color: var(--danger); }
.btn.danger:hover { border-color: var(--danger); background: var(--accent-soft); }
.settings-card {
  margin: 0 0 16px;
  padding: 16px 18px;
  border: 1px solid var(--border);
  border-radius: 16px;
  background: var(--bg-soft);
  box-shadow: var(--tile-shadow);
}
.settings-card h2 { margin: 0 0 6px; font-size: 15px; }
.settings-card p { margin: 0 0 12px; color: var(--muted); font-size: 13px; }
.settings-form {
  display: grid;
  grid-template-columns: minmax(160px, 1.4fr) auto;
  gap: 8px;
  align-items: center;
}
.settings-form.wide { grid-template-columns: minmax(180px, 1.6fr) minmax(140px, 1fr) auto; }
.settings-form input[type="text"], .settings-form select {
  min-width: 0;
  padding: 9px 12px;
  border: 1px solid var(--border);
  border-radius: 10px;
  outline: 0;
  background: var(--bg);
  color: var(--text);
  font: inherit;
  font-size: 13px;
}
.settings-form input[type="text"]:focus, .settings-form select:focus {
  border-color: var(--accent);
  box-shadow: 0 0 0 3px var(--accent-soft);
}
.settings-check {
  display: flex;
  align-items: flex-start;
  gap: 10px;
  margin: 0 0 10px;
  color: var(--text);
  font-size: 13px;
}
.settings-check input { margin-top: 2px; }
.profile-list, .sync-actions { display: flex; flex-direction: column; gap: 12px; }
.profile-card {
  display: flex;
  align-items: flex-start;
  gap: 14px;
  padding: 16px 18px;
  border: 1px solid var(--border);
  border-radius: 16px;
  background: var(--bg-soft);
  box-shadow: var(--tile-shadow);
}
.profile-card.current { border-color: var(--accent); box-shadow: 0 0 0 1px var(--accent-soft); }
.profile-main { flex: 1; min-width: 0; }
.profile-title { display: flex; align-items: center; flex-wrap: wrap; gap: 8px; }
.profile-title strong { font-size: 14.5px; }
.profile-meta { margin: 6px 0 0; color: var(--muted); font-size: 12px; word-break: break-all; }
.profile-actions { display: flex; flex-wrap: wrap; gap: 8px; margin-top: 10px; }
@media (max-width: 620px) {
  .settings-form, .settings-form.wide { grid-template-columns: 1fr; }
}
.newtab-notice {
  width: 100%;
  max-width: 640px;
  margin: 18px auto 0;
  text-align: left;
}

.shelf {
  width: 100%;
  max-width: 980px;
  margin: 0 auto;
  padding: 0 24px 48px;
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(300px, 1fr));
  gap: 18px;
}
.panel {
  background: var(--bg-soft);
  border: 1px solid var(--border);
  border-radius: 16px;
  padding: 18px 20px;
  box-shadow: var(--shadow);
  text-align: left;
  min-width: 0;
}
.panel h2 {
  display: flex;
  align-items: center;
  gap: 8px;
  margin: 0 0 8px;
  font-size: 15px;
  font-weight: 600;
  letter-spacing: -.01em;
}
.panel h2::before {
  content: "";
  width: 4px;
  height: 14px;
  flex: none;
  border-radius: 999px;
  background: var(--accent);
}
.panel .more {
  display: inline-block;
  margin-top: 8px;
  font-size: 12px;
  color: var(--accent);
}
.panel .empty { margin: 8px 0 2px; color: var(--muted); font-size: 13px; }
.list { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 2px; }
.list .item { display: flex; align-items: center; gap: 6px; min-width: 0; }
.list .row {
  flex: 1;
  min-width: 0;
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 7px 8px;
  margin: 0 -8px;
  border-radius: 9px;
  color: var(--text);
}
.list .row:hover { background: var(--bg-muted); text-decoration: none; }
.fav {
  width: 24px;
  height: 24px;
  flex: none;
  display: flex;
  align-items: center;
  justify-content: center;
  border-radius: 7px;
  background: var(--tile-bg, var(--bg-muted));
  color: var(--tile-fg, var(--muted));
  font-size: 12px;
  font-weight: 600;
}
.meta { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 1px; }
.title {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  font-size: 13px;
  line-height: 1.35;
}
.url {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  font-size: 11px;
  color: var(--muted);
}
.remove {
  flex: none;
  padding: 4px 6px;
  border-radius: 6px;
  color: var(--muted);
  font-size: 11px;
}
.remove:hover { color: var(--danger); background: var(--bg-muted); text-decoration: none; }
.privacy-card { max-width: 640px; margin-top: 30px; }
.privacy-card p { margin: 0 0 10px; }
.privacy-card .plain { margin: 0; padding-left: 18px; color: var(--muted); font-size: 13px; }
.privacy-card .plain li { margin: 4px 0; }
.content-page { min-height: 100vh; display: flex; flex-direction: column; }
.content { width: 100%; max-width: 900px; margin: 0 auto; padding: 26px 24px 56px; flex: 1; }
.page-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 16px;
  margin: 0 0 20px;
}
.page-header h1 { margin: 0; font-size: 28px; letter-spacing: -.03em; font-weight: 600; }
.page-header .count { color: var(--muted); font-size: 13px; font-weight: 400; margin-left: 8px; }
.page-actions { display: flex; flex-wrap: wrap; gap: 8px; }
.danger-link {
  flex: none;
  color: var(--danger);
  border: 1px solid var(--border);
  border-radius: 999px;
  padding: 7px 13px;
  font-size: 12px;
  background: var(--bg-soft);
}
.danger-link:hover { border-color: var(--danger); text-decoration: none; }
.notice {
  margin: 0 0 18px;
  padding: 12px 16px;
  border: 1px solid var(--border);
  border-left: 3px solid var(--accent);
  border-radius: 10px;
  background: var(--bg-soft);
  color: var(--muted);
  font-size: 13px;
}
.empty-state {
  padding: 26px 20px;
  border: 1px dashed var(--border);
  border-radius: 14px;
  color: var(--muted);
  text-align: center;
  background: var(--bg-soft);
}
.extensions { display: flex; flex-direction: column; gap: 12px; }
.extension-card {
  display: flex;
  align-items: flex-start;
  gap: 14px;
  padding: 16px 18px;
  border: 1px solid var(--border);
  border-radius: 16px;
  background: var(--bg-soft);
  box-shadow: var(--tile-shadow);
}
.ext-icon {
  width: 44px;
  height: 44px;
  flex: none;
  display: flex;
  align-items: center;
  justify-content: center;
  border-radius: 13px;
  background: var(--accent-soft);
  color: var(--accent);
  font-size: 18px;
  font-weight: 700;
}
.ext-main { flex: 1; min-width: 0; }
.ext-title { display: flex; align-items: center; flex-wrap: wrap; gap: 8px; }
.ext-title strong { font-size: 14.5px; }
.ext-version { color: var(--muted); font-size: 11.5px; }
.ext-state {
  padding: 2px 8px;
  border-radius: 999px;
  font-size: 10.5px;
  font-weight: 700;
  letter-spacing: .04em;
  text-transform: uppercase;
}
.ext-state.enabled { color: var(--accent); background: var(--accent-soft); }
.ext-state.disabled { color: var(--muted); background: var(--bg-muted); }
.ext-state.error { color: var(--danger); background: var(--accent-soft); }
.ext-description { margin: 6px 0 0; color: var(--muted); font-size: 13px; }
.ext-meta {
  margin: 6px 0 0;
  overflow: hidden;
  color: var(--muted);
  font-size: 11.5px;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.ext-error { margin: 6px 0 0; color: var(--danger); font-size: 12.5px; }
.ext-actions { display: flex; flex-wrap: wrap; gap: 8px; margin-top: 10px; }
.ext-note { margin: 8px 0 0; color: var(--muted); font-size: 13px; }
.ext-note code, .code { font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; }
.code {
  margin: 10px 0 0;
  padding: 12px 14px;
  overflow: auto;
  border: 1px solid var(--border);
  border-radius: 12px;
  background: var(--bg-muted);
  color: var(--text);
  font-size: 12px;
  line-height: 1.5;
  white-space: pre;
  tab-size: 2;
}
.page-footer {
  max-width: 900px;
  width: 100%;
  margin: 0 auto;
  padding: 18px 24px 28px;
  color: var(--muted);
  font-size: 12px;
  display: flex;
  gap: 14px;
  align-items: center;
}
.page-footer a { color: var(--muted); }
.page-footer a:hover { color: var(--text); }
@media (max-width: 620px) {
  .topbar { padding: 14px 16px; }
  .topnav a { padding: 7px 9px; }
  .hero { padding: 28px 18px 36px; }
  .hero .kite-mark { width: 62px; height: 62px; }
  .shelf { padding: 0 16px 32px; grid-template-columns: 1fr; }
  .content { padding: 18px 16px 44px; }
  .page-header { align-items: flex-start; flex-direction: column; }
  .page-footer { padding: 14px 16px 24px; }
  .recent-searches { flex-direction: column; align-items: stretch; gap: 6px; }
  .rs-label { padding-left: 2px; }
  .pin-form { grid-template-columns: 1fr; }
  .extension-card { flex-direction: column; }
}
@media (prefers-reduced-motion: reduce) {
  * { transition: none !important; }
}
)CSS";

static const char *SEARCH_JS = R"JS(
<script>
(function () {
  var SEARCH_ENGINES = {
    google: 'https://www.google.com/search?q=',
    duckduckgo: 'https://html.duckduckgo.com/html/?q=',
    startpage: 'https://www.startpage.com/sp/search?query=',
    brave: 'https://search.brave.com/search?q='
  };
  function looksLikeUrl(text) {
    if (/^(localhost|\[[0-9a-f:]+\]|[0-9]{1,3}(\.[0-9]{1,3}){3})(:[0-9]+)?([\/?#]|$)/i.test(text)) return 2;
    if (!/\s/.test(text) && /^[^\/]+\.[^\/]{2,}([\/?#].*)?$/i.test(text)) return 1;
    return 0;
  }
  function targetFor(text, engine) {
    if (/^(https?:\/\/|file:\/\/|kite:\/\/)/i.test(text)) return text;
    var bang = text.match(/^!(yt|gh|w|r)\s+([\s\S]+)$/i);
    if (bang) {
      var query = encodeURIComponent(bang[2].trim());
      var name = bang[1].toLowerCase();
      if (name === 'yt') return 'https://www.youtube.com/results?search_query=' + query;
      if (name === 'gh') return 'https://github.com/search?q=' + query;
      if (name === 'w') return 'https://en.wikipedia.org/wiki/Special:Search?search=' + query;
      return 'https://www.reddit.com/search/?q=' + query;
    }
    var kind = looksLikeUrl(text);
    if (kind === 2) return 'http://' + text;
    if (kind === 1) return 'https://' + text;
    var base = SEARCH_ENGINES[engine] || SEARCH_ENGINES.google;
    return base + encodeURIComponent(text);
  }
  document.querySelectorAll('form[data-kite-search]').forEach(function (form) {
    form.addEventListener('submit', function (event) {
      var input = form.querySelector('input[type="text"]');
      var text = input && input.value ? input.value.trim() : '';
      event.preventDefault();
      if (!text) {
        if (input) input.focus();
        return;
      }
      window.location.href = targetFor(text, form.getAttribute('data-engine'));
    });
  });
})();
</script>
)JS";

static std::string page_shell(const std::string &title, const std::string &body, const std::string &body_class = "",
                              bool force_dark = false) {
    bool dark = force_dark || settings_dark_mode();
    return "<!DOCTYPE html><html lang=\"en\"" + std::string(dark ? " data-theme=\"dark\"" : "") +
           "><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
           "<meta name=\"color-scheme\" content=\"light dark\">"
           "<title>" +
           html_escape(title) + "</title><style>" + PAGE_CSS +
           "</style></head><body" + (body_class.empty() ? "" : " class=\"" + body_class + "\"") + ">" + body +
           "</body></html>";
}

static std::string topbar(bool incognito) {
    std::string html = "<header class=\"topbar\"><a class=\"brand\" href=\"kite://newtab\">" +
                       std::string(KITE_MARK_SVG) + "<span>Kite</span></a>";
    if (incognito) {
        html += "<span class=\"badge\">Incognito</span>";
    } else {
        html += "<nav class=\"topnav\"><a href=\"kite://history\">History</a>"
                "<a href=\"kite://bookmarks\">Bookmarks</a>"
                "<a href=\"kite://extensions\">Extensions</a>"
                "<a href=\"kite://profiles\">Profiles</a>"
                "<a href=\"kite://sync\">Sync</a>"
                "<a href=\"kite://settings\">Settings</a></nav>";
    }
    return html + "</header>";
}

static std::string search_form() {
    const gchar *engine = settings_search_engine();
    std::string key = engine ? engine : "google";
    std::string action = "https://www.google.com/search";
    std::string param = "q";
    if (key == "duckduckgo") {
        action = "https://html.duckduckgo.com/html/";
    } else if (key == "startpage") {
        action = "https://www.startpage.com/sp/search";
        param = "query";
    } else if (key == "brave") {
        action = "https://search.brave.com/search";
    } else {
        key = "google";
    }

    std::string label = search_engine_display_name(key.c_str());
    std::string placeholder = "Search with " + label + " or type a URL";
    return "<form class=\"search\" action=\"" + action + "\" method=\"get\" data-kite-search data-engine=\"" +
           html_escape(key) + "\">" + std::string(SEARCH_SVG) + "<input type=\"text\" name=\"" + param +
           "\" placeholder=\"" + html_escape(placeholder) + "\" autocomplete=\"off\" autocapitalize=\"off\" "
           "spellcheck=\"false\" enterkeyhint=\"go\" aria-label=\"" +
           html_escape(placeholder) + "\"><button type=\"submit\">Search</button></form>";
}

static std::string recent_searches_bar() {
    std::vector<SearchHit> hits = recent_search_hits(8);
    if (hits.empty())
        return "";

    std::string html = "<div class=\"recent-searches\" aria-label=\"Recent searches\">"
                       "<span class=\"rs-label\">" +
                       std::string(CLOCK_SVG) + "Recent searches</span><div class=\"rs-chips\">";
    for (const auto &hit : hits) {
        html += "<a class=\"search-chip\" href=\"" + html_escape(hit.url) + "\" title=\"" +
                html_escape(hit.query) + " — " + html_escape(hit.engine) + "\"><span class=\"q\">" +
                html_escape(hit.query) + "</span><span class=\"go\">" + html_escape(hit.engine) +
                "</span></a>";
    }
    html += "</div></div>";
    return html;
}

static std::string unpin_href(const std::string &url) {
    return "kite://newtab/unpin?url=" + escape_query_value(url);
}

static std::string pinned_links_section(bool dark) {
    std::vector<PinnedLink> pins = pinned_links_list();
    int count = (int)pins.size();
    int max_pins = KITE_MAX_PINNED_LINKS;

    std::string html =
        "<section class=\"quicklinks pinned-quicklinks\"><div class=\"quicklinks-head\"><h2>Pinned</h2>"
        "<div class=\"quicklinks-links\"><span class=\"quicklinks-note\">" +
        std::to_string(count) + " of " + std::to_string(max_pins) + "</span>";
    if (count < max_pins) {
        html += "<details class=\"pin-add\"><summary><span class=\"plus\">+</span> Add pin</summary>"
                "<form class=\"pin-form\" action=\"kite://newtab/pin\" method=\"get\">"
                "<input type=\"text\" name=\"title\" placeholder=\"Title (optional)\" aria-label=\"Pin title\" "
                "autocomplete=\"off\">"
                "<input type=\"text\" name=\"url\" placeholder=\"https://example.com\" aria-label=\"Pin URL\" "
                "autocomplete=\"off\" required>"
                "<button class=\"btn primary\" type=\"submit\">Pin</button></form></details>";
    } else {
        html += "<span class=\"quicklinks-note\">Remove one to add another</span>";
    }
    html += "</div></div>";

    if (pins.empty()) {
        html += "<p class=\"quicklinks-empty\">Pin up to five favourite pages for one-click access. "
                "Choose a title and a URL above.</p>";
    } else {
        html += "<div class=\"shortcuts pinned-shortcuts\">";
        for (const auto &pin : pins) {
            std::string host = url_host(pin.url.c_str());
            std::string label = pin.title.empty() ? display_host(host) : pin.title;
            if (label.empty())
                label = pin.url;
            std::string initial(1, host_initial(host));
            html += "<div class=\"pin-card\"><a class=\"pin-remove\" href=\"" +
                    html_escape(unpin_href(pin.url)) + "\" title=\"Unpin\" aria-label=\"Unpin " +
                    html_escape(label) + "\">&times;</a>";
            html += "<a class=\"shortcut\" href=\"" + html_escape(pin.url) + "\" title=\"" +
                    html_escape(pin.url) + "\"><span class=\"tile\" style=\"" + tile_style(host, dark) + "\">" +
                    html_escape(initial) + "<span class=\"pin-flag\">" + PIN_SVG +
                    "</span></span><span class=\"name\">" + html_escape(label) + "</span></a></div>";
        }
        html += "</div>";
    }
    html += "</section>";
    return html;
}

static std::string entry_row(const HistoryEntry &entry, bool show_time, const std::string &forget_href, bool dark) {
    std::string title = entry.title.empty() ? entry.url : entry.title;
    std::string host = host_label(entry.url, entry.host);
    std::string meta = display_host(host);
    if (show_time && entry.last_visit > 0) {
        std::string when = format_visit_time(entry.last_visit);
        if (!when.empty())
            meta += (meta.empty() ? "" : " &middot; ") + when;
    }

    std::string html = "<li class=\"item\"><a class=\"row\" href=\"" + html_escape(entry.url) + "\">" +
                       favicon(host, dark) + "<span class=\"meta\"><span class=\"title\">" + html_escape(title) +
                       "</span><span class=\"url\">" + meta + "</span></span></a>";
    if (!forget_href.empty())
        html += "<a class=\"remove\" href=\"" + html_escape(forget_href) + "\">Forget</a>";
    html += "</li>";
    return html;
}

static std::string newtab_panel(const std::string &title, const std::vector<HistoryEntry> &entries,
                                const char *more_url, bool dark) {
    std::string html = "<section class=\"panel\"><h2>" + html_escape(title) + "</h2>";
    if (entries.empty()) {
        html += "<p class=\"empty\">" + html_escape(title == "Recent" ? "Pages you visit will appear here."
                                                                     : "Press Ctrl+D on a page to save it.") +
                "</p>";
    } else {
        html += "<ul class=\"list\">";
        size_t limit = std::min<size_t>(entries.size(), 6);
        for (size_t i = 0; i < limit; i++) {
            const auto &entry = entries[i];
            std::string page_title = entry.title.empty() ? entry.url : entry.title;
            std::string host = host_label(entry.url, entry.host);
            html += "<li class=\"item\"><a class=\"row\" href=\"" + html_escape(entry.url) + "\">" +
                    favicon(host, dark) + "<span class=\"meta\"><span class=\"title\">" +
                    html_escape(page_title) + "</span><span class=\"url\">" + html_escape(display_host(host)) +
                    "</span></span></a></li>";
        }
        html += "</ul>";
    }
    if (more_url)
        html += "<a class=\"more\" href=\"" + std::string(more_url) + "\">View all</a>";
    html += "</section>";
    return html;
}

static std::string newtab_page(bool incognito, const std::string &path, const gchar *query) {
    bool dark = incognito || settings_dark_mode();
    std::vector<HistoryEntry> recent;
    std::vector<HistoryEntry> bookmarks;
    bool has_shelf = false;
    std::string notice;
    bool acted = path == "/pin" || path == "/unpin";

    if (!incognito) {
        if (path == "/pin") {
            std::string raw_url = query_param(query, "url");
            std::string title = query_param(query, "title");
            if (raw_url.empty()) {
                notice = "<div class=\"notice\">Enter a URL to pin.</div>";
            } else {
                std::string url = normalize_url(raw_url);
                if (pinned_link_exists(url.c_str())) {
                    if (title.empty()) {
                        notice = "<div class=\"notice\">That link is already pinned.</div>";
                    } else if (pinned_link_add(url.c_str(), title.c_str())) {
                        notice = "<div class=\"notice\">Pinned link updated.</div>";
                    }
                } else if (pinned_links_count() >= KITE_MAX_PINNED_LINKS) {
                    notice = "<div class=\"notice\">You can pin up to five links. Remove one first.</div>";
                } else if (pinned_link_add(url.c_str(), title.c_str())) {
                    std::string host = url_host(url.c_str());
                    std::string label = display_host(host);
                    notice = "<div class=\"notice\">Pinned <strong>" + html_escape(label.empty() ? url : label) +
                             "</strong> to the start page.</div>";
                } else {
                    notice = "<div class=\"notice\">Could not pin that link.</div>";
                }
            }
        } else if (path == "/unpin") {
            std::string url = query_param(query, "url");
            if (!url.empty() && pinned_link_remove(url.c_str()))
                notice = "<div class=\"notice\">Pinned link removed.</div>";
        }

        recent = history_recent(12);
        bookmarks = bookmarks_list();
        has_shelf = !recent.empty() || !bookmarks.empty();
    }

    std::string body = topbar(incognito);
    body += std::string("<main class=\"hero") + (has_shelf ? " compact" : "") + "\">";

    if (incognito) {
        body += std::string(INCOGNITO_SVG) + "<div class=\"badge\">Incognito</div>"
                "<h1 class=\"wordmark\">You're incognito</h1>"
                "<p class=\"tagline\">Pages you view here stay out of your history and new cookies disappear when "
                "you close the tab.</p>";
        body += search_form();
        body += "<div class=\"panel privacy-card\"><p><strong>Private by default.</strong></p>"
                "<ul class=\"plain\">"
                "<li>History, bookmarks and session state are not saved from this tab.</li>"
                "<li>Permission decisions and per-site zoom are not remembered.</li>"
                "<li>Downloaded files you choose to keep are saved to disk.</li>"
                "</ul></div>";
        body += "</main>";
        body += SEARCH_JS;
        return page_shell("New incognito tab", body, "newtab-page incognito-page", true);
    }

    body += std::string(KITE_MARK_SVG) + "<h1 class=\"wordmark\">Kite</h1>"
            "<p class=\"tagline\">Search the web or type an address to get going.</p>";
    body += search_form();
    body += recent_searches_bar();
    if (!notice.empty())
        body += "<div class=\"newtab-notice\">" + notice + "</div>";
    body += pinned_links_section(dark);
    body += "</main>";

    if (has_shelf) {
        body += "<section class=\"shelf\">";
        body += newtab_panel("Recent", recent, "kite://history", dark);
        body += newtab_panel("Bookmarks", bookmarks, "kite://bookmarks", dark);
        body += "</section>";
    }

    if (acted)
        body += "<script>try{history.replaceState(null,'','kite://newtab');}catch(e){}</script>";
    body += SEARCH_JS;
    return page_shell("New tab", body, has_shelf ? "newtab-page has-shelf" : "newtab-page", false);
}

static std::string history_page(bool incognito, const std::string &path, const gchar *query) {
    if (incognito) {
        return page_shell(
            "History",
            topbar(true) +
                "<main class=\"content\"><div class=\"page-header\"><h1>History</h1></div>"
                "<div class=\"empty-state\">History is unavailable in incognito tabs.</div></main>",
            "content-page incognito-page", true);
    }

    bool dark = settings_dark_mode();
    std::string notice;
    if (path == "/clear") {
        history_clear();
        notice = "<div class=\"notice\">History cleared.</div>";
    } else if (path == "/remove") {
        std::string url = query_param(query, "url");
        if (!url.empty()) {
            history_delete_url(url.c_str());
            notice = "<div class=\"notice\">Removed that page from history.</div>";
        }
    }

    std::vector<HistoryEntry> entries = history_recent(200);
    std::string body = topbar(false);
    body += "<main class=\"content\"><div class=\"page-header\"><h1>History";
    if (!entries.empty())
        body += "<span class=\"count\">" + std::to_string(entries.size()) + " pages</span>";
    body += "</h1>";
    if (!entries.empty())
        body += "<a class=\"danger-link\" href=\"kite://history/clear\">Clear all history</a>";
    body += "</div>" + notice;

    if (entries.empty()) {
        body += "<div class=\"empty-state\">No history yet. Browse the web and pages will show up here.</div>";
    } else {
        body += "<div class=\"panel\"><ul class=\"list\">";
        for (const auto &entry : entries)
            body += entry_row(entry, true, remove_href("history", entry.url), dark);
        body += "</ul></div>";
    }
    body += "</main>";
    body += "<footer class=\"page-footer\"><a href=\"kite://newtab\">Kite start page</a>"
            "<a href=\"kite://bookmarks\">Bookmarks</a><span>Kite Browser</span></footer>";
    return page_shell("History", body, "content-page", false);
}

static std::string bookmarks_page(bool incognito, const std::string &path, const gchar *query) {
    if (incognito) {
        return page_shell(
            "Bookmarks",
            topbar(true) +
                "<main class=\"content\"><div class=\"page-header\"><h1>Bookmarks</h1></div>"
                "<div class=\"empty-state\">Bookmarks are unavailable in incognito tabs.</div></main>",
            "content-page incognito-page", true);
    }

    bool dark = settings_dark_mode();
    std::string notice;
    if (path == "/remove") {
        std::string url = query_param(query, "url");
        if (!url.empty()) {
            bookmark_remove(url.c_str());
            notice = "<div class=\"notice\">Bookmark removed.</div>";
        }
    }

    std::vector<HistoryEntry> entries = bookmarks_list();
    std::string body = topbar(false);
    body += "<main class=\"content\"><div class=\"page-header\"><h1>Bookmarks";
    if (!entries.empty())
        body += "<span class=\"count\">" + std::to_string(entries.size()) + " saved</span>";
    body += "</h1></div>" + notice;
    if (entries.empty()) {
        body += "<div class=\"empty-state\">No bookmarks yet. Press Ctrl+D on a page to save it.</div>";
    } else {
        body += "<div class=\"panel\"><ul class=\"list\">";
        for (const auto &entry : entries)
            body += entry_row(entry, false, remove_href("bookmarks", entry.url), dark);
        body += "</ul></div>";
    }
    body += "</main>";
    body += "<footer class=\"page-footer\"><a href=\"kite://newtab\">Kite start page</a>"
            "<a href=\"kite://history\">History</a><span>Kite Browser</span></footer>";
    return page_shell("Bookmarks", body, "content-page", false);
}

static std::string extension_action_href(const char *action, const std::string &id) {
    return std::string("kite://extensions/") + action + "?id=" + escape_query_value(id);
}

static std::string pending_extension_notice;
static std::string pending_extension_message;

static std::string extensions_redirect_page() {
    return "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
           "<meta http-equiv=\"refresh\" content=\"0; url=kite://extensions\">"
           "<title>Extensions</title>"
           "<script>window.location.replace('kite://extensions');</script>"
           "</head><body></body></html>";
}

static std::string extension_notice_html(const std::string &code, const std::string &message) {
    if (code == "installed")
        return "<div class=\"notice\">Extension installed and enabled.</div>";
    if (code == "install_failed")
        return "<div class=\"notice\">Could not install that folder: " +
               html_escape(message.empty() ? "unknown error" : message) + "</div>";
    if (code == "enabled")
        return "<div class=\"notice\">Extension enabled.</div>";
    if (code == "disabled")
        return "<div class=\"notice\">Extension disabled.</div>";
    if (code == "change_failed")
        return "<div class=\"notice\">Could not change that extension.</div>";
    if (code == "removed")
        return "<div class=\"notice\">Extension removed.</div>";
    if (code == "remove_failed")
        return "<div class=\"notice\">Could not remove that extension.</div>";
    if (code == "reloaded")
        return "<div class=\"notice\">Extensions reloaded.</div>";
    return "";
}

static const char *EXTENSION_EXAMPLE = R"JSON(my-extension/
  manifest.json
  content.js
  style.css

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
})JSON";

static std::string extensions_page(bool incognito, const std::string &path, const gchar *query) {
    if (incognito) {
        return page_shell(
            "Extensions",
            topbar(true) +
                "<main class=\"content\"><div class=\"page-header\"><h1>Extensions</h1></div>"
                "<div class=\"empty-state\">Extensions are managed from a normal Kite tab.</div></main>",
            "content-page incognito-page", true);
    }

    std::string notice;
    if (path == "/install") {
        std::string error;
        bool installed = false;
        bool chose_folder = false;
        GtkWidget *dialog = gtk_file_chooser_dialog_new(
            "Choose an extension folder", browser.main_window ? GTK_WINDOW(browser.main_window) : nullptr,
            GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "Cancel", GTK_RESPONSE_CANCEL, "Install", GTK_RESPONSE_ACCEPT,
            nullptr);
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), g_get_home_dir());
        if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
            chose_folder = true;
            gchar *folder = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
            installed = folder && extensions_install_from_folder(folder, &error);
            g_free(folder);
        }
        gtk_widget_destroy(dialog);

        if (chose_folder) {
            if (installed) {
                pending_extension_notice = "installed";
            } else {
                pending_extension_notice = "install_failed";
                pending_extension_message = error;
            }
        }
        return extensions_redirect_page();
    }

    if (path == "/enable" || path == "/disable") {
        std::string id = query_param(query, "id");
        if (!id.empty())
            pending_extension_notice =
                extensions_set_enabled(id.c_str(), path == "/enable") ? (path == "/enable" ? "enabled" : "disabled")
                                                                      : "change_failed";
        return extensions_redirect_page();
    }

    if (path == "/remove") {
        std::string id = query_param(query, "id");
        if (!id.empty())
            pending_extension_notice = extensions_remove(id.c_str()) ? "removed" : "remove_failed";
        return extensions_redirect_page();
    }

    if (path == "/reload") {
        extensions_reload();
        pending_extension_notice = "reloaded";
        return extensions_redirect_page();
    }

    if (!pending_extension_notice.empty()) {
        notice = extension_notice_html(pending_extension_notice, pending_extension_message);
        pending_extension_notice.clear();
        pending_extension_message.clear();
    }

    std::vector<ExtensionInfo> extensions = extensions_list();
    int enabled_count = 0;
    for (const auto &extension : extensions) {
        if (extension.enabled)
            enabled_count++;
    }

    std::string body = topbar(false);
    body += "<main class=\"content\"><div class=\"page-header\"><h1>Extensions";
    if (!extensions.empty()) {
        body += "<span class=\"count\">" + std::to_string(extensions.size()) + " installed &middot; " +
                std::to_string(enabled_count) + " enabled</span>";
    }
    body += "</h1><div class=\"page-actions\">"
            "<a class=\"btn primary\" href=\"kite://extensions/install\">Install from folder</a>"
            "<a class=\"btn\" href=\"kite://extensions/reload\">Reload all</a></div></div>" +
            notice;

    if (extensions.empty()) {
        body += "<div class=\"empty-state\">No extensions installed yet. Choose a folder that contains a "
                "<code>manifest.json</code> to add content scripts or styles to matching pages.</div>";
    } else {
        body += "<div class=\"extensions\">";
        for (const auto &extension : extensions) {
            std::string initial(1, host_initial(extension.name));
            body += "<article class=\"extension-card\"><div class=\"ext-icon\">" + html_escape(initial) +
                    "</div><div class=\"ext-main\"><div class=\"ext-title\"><strong>" +
                    html_escape(extension.name) + "</strong>";
            if (!extension.version.empty())
                body += "<span class=\"ext-version\">v" + html_escape(extension.version) + "</span>";
            if (!extension.valid)
                body += "<span class=\"ext-state error\">Error</span>";
            else if (extension.enabled)
                body += "<span class=\"ext-state enabled\">Enabled</span>";
            else
                body += "<span class=\"ext-state disabled\">Disabled</span>";
            body += "</div>";

            if (!extension.description.empty())
                body += "<p class=\"ext-description\">" + html_escape(extension.description) + "</p>";

            std::string meta;
            if (extension.script_count > 0)
                meta += std::to_string(extension.script_count) +
                        (extension.script_count == 1 ? " script" : " scripts");
            if (extension.style_count > 0) {
                if (!meta.empty())
                    meta += " · ";
                meta += std::to_string(extension.style_count) +
                        (extension.style_count == 1 ? " style" : " styles");
            }
            if (meta.empty())
                meta = "No content scripts";
            if (extension.has_background()) {
                meta += " · background";
                if (!extension.background_page.empty())
                    meta += " page";
                else
                    meta += " script";
            }
            if (!extension.permissions.empty()) {
                meta += " · permissions: ";
                for (size_t i = 0; i < extension.permissions.size(); i++) {
                    if (i)
                        meta += ", ";
                    meta += extension.permissions[i];
                }
            }
            if (!extension.host_permissions.empty()) {
                meta += " · hosts: ";
                for (size_t i = 0; i < extension.host_permissions.size(); i++) {
                    if (i)
                        meta += ", ";
                    meta += extension.host_permissions[i];
                }
            }
            meta += " · " + extension.path;
            body += "<p class=\"ext-meta\">" + html_escape(meta) + "</p>";

            if (!extension.error.empty())
                body += "<p class=\"ext-error\">" + html_escape(extension.error) + "</p>";

            body += "<div class=\"ext-actions\">";
            if (extension.valid) {
                if (extension.enabled)
                    body += "<a class=\"btn\" href=\"" + html_escape(extension_action_href("disable", extension.id)) +
                            "\">Disable</a>";
                else
                    body += "<a class=\"btn\" href=\"" + html_escape(extension_action_href("enable", extension.id)) +
                            "\">Enable</a>";
            }
            body += "<a class=\"btn danger\" href=\"" + html_escape(extension_action_href("remove", extension.id)) +
                    "\" onclick=\"return confirm('Remove this extension?');\">Remove</a>";
            body += "</div></div></article>";
        }
        body += "</div>";
    }

    body += "<section class=\"panel\" style=\"margin-top:18px\"><h2>Build a custom extension</h2>"
            "<p class=\"ext-note\">Create a folder containing a <code>manifest.json</code> plus the scripts or "
            "styles you want to inject, then choose the folder with <strong>Install from folder</strong>. Kite "
            "copies extensions to <code>" +
            html_escape(extensions_directory()) +
            "</code> and can enable, disable or remove them here.</p>"
            "<div class=\"code\">" +
            html_escape(EXTENSION_EXAMPLE) +
            "</div>"
            "<p class=\"ext-note\">Supported pieces include content scripts/styles, background pages or scripts, "
            "<code>chrome.runtime</code>, <code>chrome.storage</code>, <code>chrome.tabs</code>, "
            "<code>chrome.notifications</code>, <code>chrome.permissions</code>, and "
            "<code>kite-ext://</code> package resources. Chrome Web Store <code>.crx</code> packages and the full "
            "Chrome extension surface are not available in WebKitGTK.</p></section>";

    body += "</main>";
    body += "<footer class=\"page-footer\"><a href=\"kite://newtab\">Kite start page</a>"
            "<a href=\"kite://history\">History</a><a href=\"kite://bookmarks\">Bookmarks</a>"
            "<a href=\"kite://profiles\">Profiles</a><span>Kite Browser</span></footer>";
    return page_shell("Extensions", body, "content-page", false);
}

static std::string sync_redirect_page() {
    return "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
           "<meta http-equiv=\"refresh\" content=\"0; url=kite://sync\">"
           "<title>Sync</title>"
           "<script>window.location.replace('kite://sync');</script>"
           "</head><body></body></html>";
}

static std::string settings_redirect_page() {
    return "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
           "<meta http-equiv=\"refresh\" content=\"0; url=kite://settings?notice=saved\">"
           "<title>Settings</title>"
           "<script>window.location.replace('kite://settings?notice=saved');</script>"
           "</head><body></body></html>";
}

static std::string settings_page(bool incognito, const std::string &path, const gchar *query) {
    if (incognito) {
        return page_shell(
            "Settings",
            topbar(true) +
                "<main class=\"content\"><div class=\"page-header\"><h1>Settings</h1></div>"
                "<div class=\"empty-state\">Search settings are managed from a normal Kite tab.</div></main>",
            "content-page incognito-page", true);
    }

    if (path == "/save") {
        settings_set_search_engine(query_param(query, "engine").c_str());
        std::string fallback = query_param(query, "google_fallback");
        settings_set_google_fallback(fallback == "1" || fallback == "on" || fallback == "true");
        save_settings();
        return settings_redirect_page();
    }

    std::string notice;
    if (query_param(query, "notice") == "saved")
        notice = "<div class=\"notice\">Search settings saved.</div>";

    std::string current = settings_search_engine() ? settings_search_engine() : "google";
    std::string options;
    for (const char *key : {"google", "duckduckgo", "startpage", "brave"}) {
        options += "<option value=\"" + std::string(key) + "\"" + (current == key ? " selected" : "") + ">" +
                   html_escape(search_engine_display_name(key)) + "</option>";
    }

    std::string body = topbar(false);
    body += "<main class=\"content\"><div class=\"page-header\"><h1>Settings</h1>"
            "<span class=\"count\">Search</span></div>" +
            notice;
    body += "<section class=\"settings-card\"><h2>Search engine</h2>"
            "<p>Kite uses this engine when you type a query in the address bar or on the start page.</p>"
            "<form action=\"kite://settings/save\" method=\"get\">"
            "<div class=\"settings-form\"><select name=\"engine\">" +
            options +
            "</select><button class=\"btn primary\" type=\"submit\">Save</button></div>"
            "<label class=\"settings-check\" style=\"margin-top:12px\"><input type=\"checkbox\" "
            "name=\"google_fallback\" value=\"1\"" +
            std::string(settings_google_fallback() ? " checked" : "") +
            "><span>If Google shows a robot-verification page, retry the search with DuckDuckGo.</span></label>"
            "</form></section>";
    body += "<section class=\"settings-card\"><h2>About the Google fallback</h2>"
            "<p>Google sometimes treats automated WebKit browsers as suspicious and redirects a search to "
            "<code>/sorry/</code>. When that happens Kite recovers your query and loads DuckDuckGo results "
            "instead, with a notice above the page. Disable the option above to let the Google page load "
            "as-is.</p></section>";
    body += "</main>";
    body += "<footer class=\"page-footer\"><a href=\"kite://newtab\">Kite start page</a>"
            "<a href=\"kite://history\">History</a><a href=\"kite://bookmarks\">Bookmarks</a>"
            "<a href=\"kite://profiles\">Profiles</a><a href=\"kite://sync\">Sync</a>"
            "<span>Kite Browser</span></footer>";
    return page_shell("Settings", body, "content-page", false);
}

static std::string profiles_page(bool incognito, const std::string &path, const gchar *query) {
    if (incognito) {
        return page_shell(
            "Profiles",
            topbar(true) +
                "<main class=\"content\"><div class=\"page-header\"><h1>Profiles</h1></div>"
                "<div class=\"empty-state\">Profiles are managed from a normal Kite tab.</div></main>",
            "content-page incognito-page", true);
    }

    std::string notice;
    if (path == "/create") {
        std::string name = query_param(query, "name");
        std::string error;
        std::string new_id;
        if (name.empty()) {
            notice = "<div class=\"notice\">Enter a name for the new profile.</div>";
        } else if (profile_create(name.c_str(), &error, &new_id)) {
            notice = "<div class=\"notice\">Created profile <strong>" + html_escape(name) +
                     "</strong>. Switch to it when you are ready.</div>";
        } else {
            notice = "<div class=\"notice\">Could not create that profile" +
                     (error.empty() ? "." : ": " + html_escape(error)) + "</div>";
        }
    } else if (path == "/rename") {
        std::string id = query_param(query, "id");
        std::string name = query_param(query, "name");
        std::string error;
        if (id.empty() || name.empty()) {
            notice = "<div class=\"notice\">Choose a profile and enter a new name.</div>";
        } else if (profile_rename(id.c_str(), name.c_str(), &error)) {
            notice = "<div class=\"notice\">Renamed profile to <strong>" + html_escape(name) + "</strong>.</div>";
        } else {
            notice = "<div class=\"notice\">Could not rename that profile" +
                     (error.empty() ? "." : ": " + html_escape(error)) + "</div>";
        }
    } else if (path == "/delete") {
        std::string id = query_param(query, "id");
        std::string error;
        if (id.empty()) {
            notice = "<div class=\"notice\">Choose a profile to delete.</div>";
        } else if (profile_delete(id.c_str(), &error)) {
            notice = "<div class=\"notice\">Deleted that profile and its local data.</div>";
        } else {
            notice = "<div class=\"notice\">Could not delete that profile" +
                     (error.empty() ? "." : ": " + html_escape(error)) + "</div>";
        }
    } else if (path == "/switch") {
        std::string id = query_param(query, "id");
        if (!id.empty() && id != profile_current_id()) {
            switch_profile_and_restart(id.c_str());
            return page_shell(
                "Switching profile",
                topbar(false) +
                    "<main class=\"content\"><div class=\"page-header\"><h1>Switching profile</h1></div>"
                    "<div class=\"empty-state\">Kite is restarting with the selected profile.</div></main>",
                "content-page", false);
        }
        notice = "<div class=\"notice\">That profile is already active.</div>";
    }

    std::vector<ProfileInfo> profiles = profile_list();
    std::string body = topbar(false);
    body += "<main class=\"content\"><div class=\"page-header\"><h1>Profiles"
            "<span class=\"count\">" +
            std::to_string(profiles.size()) + " available</span></h1></div>" + notice;

    body += "<section class=\"settings-card\"><h2>Create a profile</h2>"
            "<p>Each named profile keeps its own history, bookmarks, settings, extensions and website data. "
            "Switching relaunches Kite.</p>"
            "<form class=\"settings-form\" action=\"kite://profiles/create\" method=\"get\">"
            "<input type=\"text\" name=\"name\" placeholder=\"Profile name\" required autocomplete=\"off\">"
            "<button class=\"btn primary\" type=\"submit\">Create</button></form></section>";

    body += "<div class=\"profile-list\">";
    for (const auto &profile : profiles) {
        std::string initial(1, host_initial(profile.name));
        body += "<article class=\"profile-card" + std::string(profile.current ? " current" : "") +
                "\"><div class=\"ext-icon\">" + html_escape(initial) +
                "</div><div class=\"profile-main\"><div class=\"profile-title\"><strong>" +
                html_escape(profile.name) + "</strong>";
        if (profile.is_default)
            body += "<span class=\"ext-state enabled\">Default</span>";
        if (profile.current)
            body += "<span class=\"ext-state enabled\">Current</span>";
        body += "</div><p class=\"profile-meta\">id: " + html_escape(profile.id) + "</p>";

        body += "<div class=\"profile-actions\">";
        if (!profile.current) {
            body += "<a class=\"btn primary\" href=\"kite://profiles/switch?id=" + escape_query_value(profile.id) +
                    "\">Switch</a>";
        }
        if (!profile.is_default) {
            body += "<form class=\"settings-form\" action=\"kite://profiles/rename\" method=\"get\">"
                    "<input type=\"hidden\" name=\"id\" value=\"" + html_escape(profile.id) + "\">"
                    "<input type=\"text\" name=\"name\" value=\"" + html_escape(profile.name) +
                    "\" required autocomplete=\"off\">"
                    "<button class=\"btn\" type=\"submit\">Rename</button></form>";
        }
        if (!profile.is_default && !profile.current) {
            body += "<a class=\"btn danger\" href=\"kite://profiles/delete?id=" + escape_query_value(profile.id) +
                    "\">Delete</a>";
        }
        body += "</div></div></article>";
    }
    body += "</div>";

    body += "<section class=\"settings-card\" style=\"margin-top:18px\"><h2>Paths</h2>"
            "<p>Default profile data stays in the legacy Kite folders. Named profiles use isolated subfolders.</p>"
            "<p class=\"ext-meta\">Data: <code>" +
            html_escape(profile_data_dir()) + "</code></p>"
            "<p class=\"ext-meta\">Config: <code>" +
            html_escape(profile_config_dir()) + "</code></p></section>";

    body += "</main><footer class=\"page-footer\">"
            "<a href=\"kite://sync\">Sync</a><a href=\"kite://extensions\">Extensions</a>"
            "<span>Kite Browser</span></footer>";
    return page_shell("Profiles", body, "content-page", false);
}

static std::string sync_page(bool incognito, const std::string &path, const gchar *query) {
    if (incognito) {
        return page_shell(
            "Sync",
            topbar(true) +
                "<main class=\"content\"><div class=\"page-header\"><h1>Sync</h1></div>"
                "<div class=\"empty-state\">Sync is managed from a normal Kite tab.</div></main>",
            "content-page incognito-page", true);
    }

    std::string notice;
    if (path == "/save") {
        std::string folder = query_param(query, "folder");
        bool on_startup = query_param(query, "on_startup") == "1" || query_param(query, "on_startup") == "on";
        bool include_settings =
            query_param(query, "sync_settings") == "1" || query_param(query, "sync_settings") == "on";
        settings_set_sync_folder(folder.c_str());
        settings_set_sync_on_startup(on_startup);
        settings_set_sync_settings(include_settings);
        save_settings();
        notice = "<div class=\"notice\">Sync settings saved.</div>";
    } else if (path == "/choose") {
        GtkWidget *dialog = gtk_file_chooser_dialog_new(
            "Choose a sync folder", browser.main_window ? GTK_WINDOW(browser.main_window) : nullptr,
            GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "Cancel", GTK_RESPONSE_CANCEL, "Use folder", GTK_RESPONSE_ACCEPT,
            nullptr);
        const gchar *current = settings_sync_folder();
        if (current && *current)
            gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), current);
        else
            gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), g_get_home_dir());
        if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
            gchar *folder = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
            if (folder) {
                settings_set_sync_folder(folder);
                save_settings();
                g_free(folder);
            }
        }
        gtk_widget_destroy(dialog);
        return sync_redirect_page();
    } else if (path == "/now") {
        const gchar *folder = settings_sync_folder();
        if (!folder || !*folder) {
            notice = "<div class=\"notice\">Choose a sync folder first.</div>";
        } else {
            SyncResult result;
            std::string error;
            if (sync_run(folder, settings_sync_settings(), &result, &error)) {
                notice = "<div class=\"notice\">Synced with <code>" + html_escape(folder) + "</code>: " +
                         std::to_string(result.bookmarks_added) + " bookmark(s), " +
                         std::to_string(result.pins_added) + " pin(s), " +
                         std::to_string(result.history_added) + " history entr" +
                         (result.history_added == 1 ? "y" : "ies") +
                         (result.settings_applied ? ", settings applied" : "") + ".</div>";
            } else {
                notice = "<div class=\"notice\">Sync failed" +
                         (error.empty() ? "." : ": " + html_escape(error)) + "</div>";
            }
        }
    } else if (path == "/export") {
        const gchar *folder = settings_sync_folder();
        if (!folder || !*folder) {
            notice = "<div class=\"notice\">Choose a sync folder first.</div>";
        } else {
            g_mkdir_with_parents(folder, 0700);
            gchar *path_out = g_build_filename(folder, sync_file_name(), nullptr);
            std::string error;
            if (sync_write_file(path_out, &error)) {
                notice = "<div class=\"notice\">Exported current profile data to <code>" +
                         html_escape(path_out) + "</code>.</div>";
            } else {
                notice = "<div class=\"notice\">Export failed" +
                         (error.empty() ? "." : ": " + html_escape(error)) + "</div>";
            }
            g_free(path_out);
        }
    }

    std::string folder = settings_sync_folder() ? settings_sync_folder() : "";
    std::string body = topbar(false);
    body += "<main class=\"content\"><div class=\"page-header\"><h1>Sync</h1>"
            "<div class=\"page-actions\">"
            "<a class=\"btn\" href=\"kite://sync/choose\">Choose folder</a>"
            "<a class=\"btn primary\" href=\"kite://sync/now\">Sync now</a>"
            "<a class=\"btn\" href=\"kite://sync/export\">Export only</a>"
            "</div></div>" +
            notice;

    body += "<section class=\"settings-card\"><h2>Shared folder</h2>"
            "<p>Kite writes <code>" +
            html_escape(sync_file_name()) +
            "</code> into a folder you control. Merge keeps local titles, newest history visits, "
            "and the five-pin limit.</p>"
            "<form action=\"kite://sync/save\" method=\"get\">"
            "<div class=\"settings-form\">"
            "<input type=\"text\" name=\"folder\" value=\"" +
            html_escape(folder) +
            "\" placeholder=\"/path/to/shared/folder\" autocomplete=\"off\">"
            "<button class=\"btn primary\" type=\"submit\">Save</button></div>"
            "<label class=\"settings-check\"><input type=\"checkbox\" name=\"on_startup\" value=\"1\"" +
            std::string(settings_sync_on_startup() ? " checked" : "") +
            "> Sync on startup</label>"
            "<label class=\"settings-check\"><input type=\"checkbox\" name=\"sync_settings\" value=\"1\"" +
            std::string(settings_sync_settings() ? " checked" : "") +
            "> Include selected settings (homepage, search engine, Google fallback, dark mode, restore session, "
            "ad blocking, tracking-parameter stripping, clear on exit)</label>"
            "</form></section>";

    body += "<section class=\"settings-card\"><h2>What syncs</h2>"
            "<p>Bookmarks, pinned links, history, homepage, search engine, Google fallback, dark mode, "
            "restore-session, ad blocking, tracking-parameter stripping and clear-on-exit. Cookies, passwords "
            "and extensions stay local for now.</p></section>";

    body += "</main><footer class=\"page-footer\">"
            "<a href=\"kite://profiles\">Profiles</a><a href=\"kite://bookmarks\">Bookmarks</a>"
            "<span>Kite Browser</span></footer>";
    return page_shell("Sync", body, "content-page", false);
}

static std::string not_found_page() {
    return page_shell("Page not found",
                      topbar(false) +
                          "<main class=\"content\"><div class=\"page-header\"><h1>Page not found</h1></div>"
                          "<div class=\"empty-state\">This internal page does not exist.<br><br>"
                          "<a href=\"kite://newtab\">Open the Kite start page</a></div></main>",
                      "content-page", false);
}

static void on_kite_request(WebKitURISchemeRequest *request, gpointer user_data) {
    const gchar *uri = webkit_uri_scheme_request_get_uri(request);
    WebKitWebView *web_view = webkit_uri_scheme_request_get_web_view(request);
    gboolean incognito = web_view ? webview_is_incognito(web_view) : FALSE;

    GError *error = nullptr;
    GUri *parsed = uri ? g_uri_parse(uri, G_URI_FLAGS_NONE, &error) : nullptr;
    if (!parsed) {
        g_clear_error(&error);
        std::string html = page_shell("Bad request",
                                      topbar(false) +
                                          "<main class=\"content\"><div class=\"page-header\"><h1>Bad request</h1>"
                                          "</div><div class=\"empty-state\">This internal URL could not be read."
                                          "</div></main>",
                                      "content-page", false);
        GBytes *bytes = g_bytes_new(html.data(), html.size());
        GInputStream *stream = g_memory_input_stream_new_from_bytes(bytes);
        webkit_uri_scheme_request_finish(request, stream, (gint64)g_bytes_get_size(bytes),
                                         "text/html; charset=utf-8");
        g_object_unref(stream);
        g_bytes_unref(bytes);
        return;
    }

    const gchar *host = g_uri_get_host(parsed);
    const gchar *path = g_uri_get_path(parsed);
    const gchar *query = g_uri_get_query(parsed);
    std::string host_name = host ? host : "";
    std::string path_name = path ? path : "";

    std::string html;
    if (host_name.empty() || host_name == "newtab")
        html = newtab_page(incognito, path_name, query);
    else if (host_name == "history")
        html = history_page(incognito, path_name, query);
    else if (host_name == "bookmarks")
        html = bookmarks_page(incognito, path_name, query);
    else if (host_name == "extensions")
        html = extensions_page(incognito, path_name, query);
    else if (host_name == "profiles")
        html = profiles_page(incognito, path_name, query);
    else if (host_name == "sync")
        html = sync_page(incognito, path_name, query);
    else if (host_name == "settings")
        html = settings_page(incognito, path_name, query);
    else
        html = not_found_page();

    g_uri_unref(parsed);

    GBytes *bytes = g_bytes_new(html.data(), html.size());
    GInputStream *stream = g_memory_input_stream_new_from_bytes(bytes);
    webkit_uri_scheme_request_finish(request, stream, (gint64)g_bytes_get_size(bytes),
                                     "text/html; charset=utf-8");
    g_object_unref(stream);
    g_bytes_unref(bytes);
}

void init_kite_pages(WebKitWebContext *context) {
    if (!context)
        return;
    webkit_web_context_register_uri_scheme(context, "kite", on_kite_request, nullptr, nullptr);
}
