#include "omnibox.h"

#include <glib.h>
#include <glib/gstdio.h>

#include <cstdio>
#include <string>

#include "history.h"
#include "kite_urls.h"
#include "settings.h"

static int failures = 0;

static void check(bool condition, const char *label) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", label);
        failures++;
    }
}

static OmniboxKind kind_for(const std::vector<OmniboxSuggestion> &suggestions, const std::string &url) {
    for (const auto &suggestion : suggestions) {
        if (suggestion.url == url)
            return suggestion.kind;
    }
    return OmniboxKind::History;
}

int main() {
    GError *error = nullptr;
    gchar *tmp = g_dir_make_tmp("kite-omnibox-XXXXXX", &error);
    if (!tmp) {
        std::fprintf(stderr, "Could not create temp dir: %s\n", error ? error->message : "unknown error");
        if (error)
            g_error_free(error);
        return 1;
    }
    gchar *db_path = g_build_filename(tmp, "kite.db", nullptr);
    if (!history_init(db_path)) {
        std::fprintf(stderr, "Could not open history database\n");
        return 1;
    }

    history_record_visit("https://example.com/docs", "");
    history_update_title("https://example.com/docs", "Example docs");
    history_record_visit("https://other.test/", "Other");
    bookmark_add("https://bookmarked.test/", "Example bookmark");
    pinned_link_add("https://pinned.test/", "Example pin");

    std::vector<OmniboxSuggestion> suggestions = omnibox_suggestions("exam", true, 10);
    check(!suggestions.empty(), "suggestions returned");
    if (!suggestions.empty()) {
        check(suggestions[0].kind == OmniboxKind::Search, "search row comes first");
        check(suggestions[0].url.find("google.com/search") != std::string::npos, "search row uses Google");
    }
    check(kind_for(suggestions, "https://example.com/docs") == OmniboxKind::History, "history entry found");
    check(kind_for(suggestions, "https://bookmarked.test/") == OmniboxKind::Bookmark, "bookmark found");
    check(kind_for(suggestions, "https://pinned.test/") == OmniboxKind::Pin, "pin found");

    suggestions = omnibox_suggestions("https://example.com/docs", true, 10);
    check(!suggestions.empty() && suggestions[0].kind == OmniboxKind::Open, "address row comes first for a URL");
    check(!suggestions.empty() && suggestions[0].url == "https://example.com/docs", "address row keeps the URL");

    suggestions = omnibox_suggestions("hist", true, 10);
    check(kind_for(suggestions, KITE_HISTORY_URL) == OmniboxKind::Page, "built-in history page found");

    suggestions = omnibox_suggestions("settings", true, 10);
    check(kind_for(suggestions, KITE_SETTINGS_URL) == OmniboxKind::Page, "built-in settings page found");

    settings_set_search_engine("duckduckgo");
    suggestions = omnibox_suggestions("exam", true, 10);
    check(!suggestions.empty() && suggestions[0].kind == OmniboxKind::Search, "search row still comes first");
    check(!suggestions.empty() && suggestions[0].url.find("duckduckgo.com") != std::string::npos,
          "search row follows configured engine");
    settings_set_search_engine("google");

    suggestions = omnibox_suggestions("exam", false, 10);
    bool has_personal = false;
    for (const auto &suggestion : suggestions) {
        if (suggestion.kind == OmniboxKind::History || suggestion.kind == OmniboxKind::Bookmark ||
            suggestion.kind == OmniboxKind::Pin)
            has_personal = true;
    }
    check(!has_personal, "incognito suggestions exclude personal entries");
    check(!suggestions.empty() && suggestions[0].kind == OmniboxKind::Search, "incognito keeps search row");

    suggestions = omnibox_suggestions("", true, 10);
    check(suggestions.empty(), "empty query returns no suggestions");

    history_close();
    g_remove(db_path);
    g_free(db_path);
    g_rmdir(tmp);
    g_free(tmp);

    if (failures) {
        std::fprintf(stderr, "%d omnibox test(s) failed\n", failures);
        return 1;
    }
    std::printf("omnibox tests passed\n");
    return 0;
}
