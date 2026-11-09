#include "kite_pages.h"

#include <glib.h>

#include <string>

#include "browser_window.h"
#include "history.h"

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

static std::string page_shell(const std::string &title, const std::string &body) {
    return "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
           "<title>" +
           html_escape(title) +
           "</title><style>"
           ":root{color-scheme:light dark}"
           "body{font-family:system-ui,-apple-system,'Segoe UI',sans-serif;margin:0;background:#fafafa;color:#202124}"
           "main{max-width:780px;margin:0 auto;padding:56px 24px 80px}"
           "h1{font-size:34px;margin:0 0 8px;letter-spacing:-.03em}"
           "h2{font-size:18px;margin:34px 0 12px}"
           ".lead{color:#5f6368;margin:0 0 28px;font-size:16px}"
           ".cards{display:flex;gap:12px;flex-wrap:wrap}"
           ".card{display:flex;flex-direction:column;gap:4px;flex:1;min-width:180px;padding:16px 18px;"
           "background:#fff;border:1px solid #dadce0;border-radius:12px;text-decoration:none;color:#202124}"
           ".card strong{font-size:15px}.card span{color:#5f6368;font-size:13px}"
           ".card:hover{border-color:#1a73e8;text-decoration:none}"
           "ul.items{list-style:none;padding:0;margin:0}"
           "ul.items li{padding:12px 0;border-bottom:1px solid #e8eaed;display:flex;justify-content:space-between;gap:16px}"
           ".entry-title{color:#1a73e8;text-decoration:none;font-size:15px;word-break:break-word}"
           ".entry-title:hover{text-decoration:underline}"
           ".meta{color:#5f6368;font-size:12px;margin-top:4px;word-break:break-all}"
           ".remove{color:#5f6368;font-size:12px;text-decoration:none;white-space:nowrap}"
           ".remove:hover{color:#c5221f;text-decoration:underline}"
           ".notice{background:#fff7e6;border:1px solid #f3d38a;border-radius:10px;padding:14px 16px;"
           "color:#5f4b1f;margin:0 0 24px;font-size:14px}"
           ".notice.incognito{background:#1a0b2e;border-color:#8e44ad;color:#d7c2ff}"
           ".toolbar{display:flex;justify-content:space-between;align-items:center;margin:0 0 24px;gap:16px}"
           ".toolbar a{font-size:13px;color:#1a73e8;text-decoration:none}"
           ".danger{color:#c5221f !important}"
           ".empty{color:#5f6368;background:#fff;border:1px dashed #dadce0;border-radius:10px;padding:20px;text-align:center}"
           "footer{margin-top:48px;color:#80868b;font-size:12px}"
           "</style></head><body><main>" +
           body + "<footer>Kite Browser</footer></main></body></html>";
}

static std::string entry_row(const HistoryEntry &entry, bool show_time, const std::string &forget_href) {
    std::string title = entry.title.empty() ? entry.url : entry.title;
    std::string html = "<li><div><a class=\"entry-title\" href=\"" + html_escape(entry.url) + "\">" +
                       html_escape(title) + "</a><div class=\"meta\">" + html_escape(entry.url);
    if (show_time && entry.last_visit > 0)
        html += " &middot; " + html_escape(format_visit_time(entry.last_visit));
    html += "</div></div>";
    if (!forget_href.empty())
        html += "<a class=\"remove\" href=\"" + html_escape(forget_href) + "\">Forget</a>";
    html += "</li>";
    return html;
}

static std::string newtab_page(bool incognito) {
    std::string body = "<h1>Kite</h1>"
                       "<p class=\"lead\">Type in the address bar to search or enter an address.</p>";

    if (incognito) {
        body += "<div class=\"notice incognito\">You are browsing in incognito. History, bookmarks, session state, "
                "permission decisions, and per-site zoom are not saved from this tab.</div>";
        return page_shell("New tab", body);
    }

    body += "<div class=\"cards\">"
            "<a class=\"card\" href=\"kite://history\"><strong>History</strong><span>Recently visited pages</span></a>"
            "<a class=\"card\" href=\"kite://bookmarks\"><strong>Bookmarks</strong><span>Your saved pages</span></a>"
            "</div>";

    std::vector<HistoryEntry> recent = history_recent(8);
    if (!recent.empty()) {
        body += "<h2>Recent</h2><ul class=\"items\">";
        for (const auto &entry : recent)
            body += entry_row(entry, true, "");
        body += "</ul>";
    }

    std::vector<HistoryEntry> bookmarks = bookmarks_list();
    if (!bookmarks.empty()) {
        body += "<h2>Bookmarks</h2><ul class=\"items\">";
        for (size_t i = 0; i < bookmarks.size() && i < 8; i++)
            body += entry_row(bookmarks[i], false, "");
        body += "</ul>";
    }

    return page_shell("New tab", body);
}

static std::string history_page(bool incognito, const std::string &path, const gchar *query) {
    if (incognito) {
        return page_shell(
            "History",
            "<h1>History</h1><div class=\"notice incognito\">History is unavailable in incognito tabs.</div>"
            "<p><a href=\"kite://newtab\">Back to new tab</a></p>");
    }

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
    std::string body = "<div class=\"toolbar\"><h1>History</h1>";
    if (!entries.empty())
        body += "<a class=\"danger\" href=\"kite://history/clear\">Clear all history</a>";
    body += "</div>" + notice;

    if (entries.empty()) {
        body += "<div class=\"empty\">No history yet. Browse the web and pages will show up here.</div>";
    } else {
        body += "<ul class=\"items\">";
        for (const auto &entry : entries)
            body += entry_row(entry, true, remove_href("history", entry.url));
        body += "</ul>";
    }
    body += "<p><a href=\"kite://newtab\">Back to new tab</a></p>";
    return page_shell("History", body);
}

static std::string bookmarks_page(bool incognito, const std::string &path, const gchar *query) {
    if (incognito) {
        return page_shell(
            "Bookmarks",
            "<h1>Bookmarks</h1><div class=\"notice incognito\">Bookmarks are unavailable in incognito tabs.</div>"
            "<p><a href=\"kite://newtab\">Back to new tab</a></p>");
    }

    std::string notice;
    if (path == "/remove") {
        std::string url = query_param(query, "url");
        if (!url.empty()) {
            bookmark_remove(url.c_str());
            notice = "<div class=\"notice\">Bookmark removed.</div>";
        }
    }

    std::vector<HistoryEntry> entries = bookmarks_list();
    std::string body = "<div class=\"toolbar\"><h1>Bookmarks</h1></div>" + notice;
    if (entries.empty()) {
        body += "<div class=\"empty\">No bookmarks yet. Press Ctrl+D on a page to save it.</div>";
    } else {
        body += "<ul class=\"items\">";
        for (const auto &entry : entries)
            body += entry_row(entry, false, remove_href("bookmarks", entry.url));
        body += "</ul>";
    }
    body += "<p><a href=\"kite://newtab\">Back to new tab</a></p>";
    return page_shell("Bookmarks", body);
}

static std::string not_found_page() {
    return page_shell("Page not found",
                      "<h1>Page not found</h1><p class=\"lead\">This internal page does not exist.</p>"
                      "<p><a href=\"kite://newtab\">Open the new tab page</a></p>");
}

static void on_kite_request(WebKitURISchemeRequest *request, gpointer user_data) {
    const gchar *uri = webkit_uri_scheme_request_get_uri(request);
    WebKitWebView *web_view = webkit_uri_scheme_request_get_web_view(request);
    gboolean incognito = web_view ? webview_is_incognito(web_view) : FALSE;

    GError *error = nullptr;
    GUri *parsed = uri ? g_uri_parse(uri, G_URI_FLAGS_NONE, &error) : nullptr;
    if (!parsed) {
        g_clear_error(&error);
        std::string html = page_shell("Bad request", "<h1>Bad request</h1>");
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
