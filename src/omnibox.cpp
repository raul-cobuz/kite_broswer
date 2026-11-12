#include "omnibox.h"

#include <glib.h>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "history.h"
#include "kite_urls.h"
#include "settings.h"
#include "url_utils.h"

namespace {

struct Candidate {
    OmniboxSuggestion suggestion;
    int score = 0;
};

std::string lower(const std::string &text) {
    gchar *result = g_ascii_strdown(text.c_str(), -1);
    std::string value = result ? result : "";
    g_free(result);
    return value;
}

bool contains(const std::string &haystack, const std::string &needle) {
    return lower(haystack).find(needle) != std::string::npos;
}

int match_bonus(const OmniboxSuggestion &suggestion, const std::string &needle) {
    std::string title = lower(suggestion.title);
    std::string url = lower(suggestion.url);
    int bonus = 0;
    if (title.rfind(needle, 0) == 0)
        bonus += 8;
    else if (title.find(needle) != std::string::npos)
        bonus += 4;
    if (url.rfind(needle, 0) == 0)
        bonus += 6;
    else if (url.find(needle) != std::string::npos)
        bonus += 2;
    return bonus;
}

int kind_bonus(OmniboxKind kind) {
    switch (kind) {
    case OmniboxKind::Pin:
        return 30;
    case OmniboxKind::Bookmark:
        return 20;
    case OmniboxKind::History:
        return 10;
    case OmniboxKind::Page:
        return 5;
    default:
        return 0;
    }
}

void add_candidate(std::vector<Candidate> &candidates, std::unordered_set<std::string> &seen,
                   OmniboxSuggestion suggestion, int base_score) {
    if (suggestion.url.empty())
        return;
    if (suggestion.title.empty())
        suggestion.title = suggestion.url;
    if (!seen.insert(suggestion.url).second)
        return;
    Candidate candidate;
    candidate.suggestion = suggestion;
    candidate.score = base_score + kind_bonus(suggestion.kind);
    candidates.push_back(candidate);
}

std::vector<OmniboxSuggestion> builtin_pages() {
    return {
        {OmniboxKind::Page, "Kite start page", KITE_NEWTAB_URL, "New tab"},
        {OmniboxKind::Page, "History", KITE_HISTORY_URL, "Kite page"},
        {OmniboxKind::Page, "Bookmarks", KITE_BOOKMARKS_URL, "Kite page"},
        {OmniboxKind::Page, "Extensions", KITE_EXTENSIONS_URL, "Kite page"},
        {OmniboxKind::Page, "Profiles", KITE_PROFILES_URL, "Kite page"},
        {OmniboxKind::Page, "Sync", KITE_SYNC_URL, "Kite page"},
        {OmniboxKind::Page, "Settings", KITE_SETTINGS_URL, "Kite page"},
    };
}

bool suggestion_matches(const OmniboxSuggestion &suggestion, const std::string &needle) {
    return contains(suggestion.title, needle) || contains(suggestion.url, needle) ||
           contains(suggestion.detail, needle);
}

} // namespace

std::vector<OmniboxSuggestion> omnibox_suggestions(const gchar *text, bool include_personal, int limit) {
    std::vector<OmniboxSuggestion> results;
    if (!text || !*text)
        return results;

    std::string query = text;
    std::string needle = lower(query);
    if (limit <= 0)
        limit = 10;

    std::vector<Candidate> candidates;
    std::unordered_set<std::string> seen;

    std::string search_target = search_url(query);
    std::string resolved = normalize_url(query);
    if (!resolved.empty() && resolved != search_target) {
        OmniboxSuggestion open;
        open.kind = OmniboxKind::Open;
        open.title = "Open " + resolved;
        open.url = resolved;
        open.detail = "Address";
        add_candidate(candidates, seen, open, 1020);
    }

    const gchar *engine = settings_search_engine();
    const gchar *engine_name = search_engine_display_name(engine);
    OmniboxSuggestion search;
    search.kind = OmniboxKind::Search;
    search.title = std::string("Search ") + engine_name + " for \"" + query + "\"";
    search.url = search_target;
    search.detail = engine_name;
    add_candidate(candidates, seen, search, 1000);

    for (const auto &page : builtin_pages()) {
        if (suggestion_matches(page, needle))
            add_candidate(candidates, seen, page, 40 + match_bonus(page, needle));
    }

    if (include_personal) {
        std::unordered_map<std::string, OmniboxKind> known;
        for (const auto &pin : pinned_links_list())
            known[pin.url] = OmniboxKind::Pin;

        for (const auto &bookmark : bookmarks_list()) {
            if (known.find(bookmark.url) == known.end())
                known[bookmark.url] = OmniboxKind::Bookmark;
        }

        for (const auto &entry : history_completions(query.c_str(), 30)) {
            OmniboxSuggestion suggestion;
            auto it = known.find(entry.url);
            suggestion.kind = it != known.end() ? it->second : OmniboxKind::History;
            suggestion.title = entry.title;
            suggestion.url = entry.url;
            suggestion.detail = entry.host;
            if (suggestion_matches(suggestion, needle))
                add_candidate(candidates, seen, suggestion, 20 + match_bonus(suggestion, needle));
        }

        for (const auto &pin : pinned_links_list()) {
            OmniboxSuggestion suggestion;
            suggestion.kind = OmniboxKind::Pin;
            suggestion.title = pin.title;
            suggestion.url = pin.url;
            suggestion.detail = url_host(pin.url.c_str());
            if (suggestion_matches(suggestion, needle))
                add_candidate(candidates, seen, suggestion, 20 + match_bonus(suggestion, needle));
        }

        for (const auto &bookmark : bookmarks_list()) {
            OmniboxSuggestion suggestion;
            suggestion.kind = known[bookmark.url] == OmniboxKind::Pin ? OmniboxKind::Pin : OmniboxKind::Bookmark;
            suggestion.title = bookmark.title;
            suggestion.url = bookmark.url;
            suggestion.detail = url_host(bookmark.url.c_str());
            if (suggestion_matches(suggestion, needle))
                add_candidate(candidates, seen, suggestion, 20 + match_bonus(suggestion, needle));
        }
    }

    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate &a, const Candidate &b) { return a.score > b.score; });

    for (const auto &candidate : candidates) {
        if ((int)results.size() >= limit)
            break;
        results.push_back(candidate.suggestion);
    }
    return results;
}

const char *omnibox_kind_label(OmniboxKind kind) {
    switch (kind) {
    case OmniboxKind::Search:
        return "Search";
    case OmniboxKind::Open:
        return "Address";
    case OmniboxKind::Page:
        return "Kite";
    case OmniboxKind::History:
        return "History";
    case OmniboxKind::Bookmark:
        return "Bookmark";
    case OmniboxKind::Pin:
        return "Pinned";
    }
    return "";
}
