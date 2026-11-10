#include "kite_pages.h"

#include <glib.h>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

#include "browser_window.h"
#include "history.h"
#include "settings.h"
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

static const char *STAR_SVG =
    "<svg viewBox=\"0 0 24 24\" aria-hidden=\"true\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"1.9\" "
    "stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"m12 3.8 2.5 5.1 5.6.8-4 3.9.9 5.6-5-2.6-5 "
    "2.6.9-5.6-4-3.9 5.6-.8Z\"/></svg>";

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
  margin: 0 0 8px;
  font-size: 15px;
  font-weight: 600;
  letter-spacing: -.01em;
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
}
@media (prefers-reduced-motion: reduce) {
  * { transition: none !important; }
}
)CSS";

static const char *SEARCH_JS = R"JS(
<script>
(function () {
  function looksLikeUrl(text) {
    if (/^(localhost|\[[0-9a-f:]+\]|[0-9]{1,3}(\.[0-9]{1,3}){3})(:[0-9]+)?([\/?#]|$)/i.test(text)) return 2;
    if (!/\s/.test(text) && /^[^\/]+\.[^\/]{2,}([\/?#].*)?$/i.test(text)) return 1;
    return 0;
  }
  function targetFor(text) {
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
    return 'https://duckduckgo.com/?q=' + encodeURIComponent(text);
  }
  document.querySelectorAll('form[data-kite-search]').forEach(function (form) {
    form.addEventListener('submit', function (event) {
      var input = form.querySelector('input[name="q"]');
      var text = input && input.value ? input.value.trim() : '';
      event.preventDefault();
      if (!text) {
        if (input) input.focus();
        return;
      }
      window.location.href = targetFor(text);
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
                "<a href=\"kite://bookmarks\">Bookmarks</a></nav>";
    }
    return html + "</header>";
}

static std::string search_form() {
    return "<form class=\"search\" action=\"https://duckduckgo.com/\" method=\"get\" data-kite-search>" +
           std::string(SEARCH_SVG) +
           "<input type=\"text\" name=\"q\" placeholder=\"Search with DuckDuckGo or type a URL\" "
           "autocomplete=\"off\" autocapitalize=\"off\" spellcheck=\"false\" enterkeyhint=\"go\" "
           "aria-label=\"Search with DuckDuckGo or type a URL\">"
           "<button type=\"submit\">Search</button></form>";
}

static std::string top_sites(bool dark) {
    std::string html;
    std::vector<HistoryEntry> recent = history_recent(64);
    std::unordered_set<std::string> seen_hosts;
    int added = 0;

    for (const auto &entry : recent) {
        if (added >= 6)
            break;
        std::string host = host_label(entry.url, entry.host);
        if (host.empty() || !seen_hosts.insert(host).second)
            continue;
        std::string label = display_host(host);
        std::string initial(1, host_initial(host));
        html += "<a class=\"shortcut\" href=\"" + html_escape(entry.url) + "\"><span class=\"tile\" style=\"" +
                tile_style(host, dark) + "\">" + html_escape(initial) + "</span><span class=\"name\">" +
                html_escape(label) + "</span></a>";
        added++;
    }
    return html;
}

static std::string shortcuts(bool dark) {
    std::string html = "<div class=\"shortcuts\">";
    html += "<a class=\"shortcut\" href=\"kite://history\"><span class=\"tile fixed\">" + std::string(CLOCK_SVG) +
            "</span><span class=\"name\">History</span></a>";
    html += "<a class=\"shortcut\" href=\"kite://bookmarks\"><span class=\"tile fixed\">" + std::string(STAR_SVG) +
            "</span><span class=\"name\">Bookmarks</span></a>";
    html += top_sites(dark);
    html += "</div>";
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

static std::string newtab_page(bool incognito) {
    bool dark = incognito || settings_dark_mode();
    std::vector<HistoryEntry> recent;
    std::vector<HistoryEntry> bookmarks;
    bool has_shelf = false;
    if (!incognito) {
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
    body += shortcuts(dark);
    body += "</main>";

    if (has_shelf) {
        body += "<section class=\"shelf\">";
        body += newtab_panel("Recent", recent, "kite://history", dark);
        body += newtab_panel("Bookmarks", bookmarks, "kite://bookmarks", dark);
        body += "</section>";
    }

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
        html = newtab_page(incognito);
    else if (host_name == "history")
        html = history_page(incognito, path_name, query);
    else if (host_name == "bookmarks")
        html = bookmarks_page(incognito, path_name, query);
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
