#pragma once

#include <glib.h>

#include <string>
#include <vector>

enum class OmniboxKind {
    Search,
    Open,
    Page,
    History,
    Bookmark,
    Pin,
};

struct OmniboxSuggestion {
    OmniboxKind kind = OmniboxKind::History;
    std::string title;
    std::string url;
    std::string detail;
};

// Ranked, deduplicated omnibox suggestions. Personal entries (history,
// bookmarks and pins) can be excluded for incognito tabs.
std::vector<OmniboxSuggestion> omnibox_suggestions(const gchar *text, bool include_personal, int limit = 10);

const char *omnibox_kind_label(OmniboxKind kind);
