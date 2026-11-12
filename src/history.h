#pragma once

#include <glib.h>

#include <string>
#include <vector>

struct HistoryEntry {
    std::string url;
    std::string title;
    std::string host;
    gint64 last_visit = 0;
    int visit_count = 0;
};

struct PinnedLink {
    std::string url;
    std::string title;
};

constexpr int KITE_MAX_PINNED_LINKS = 5;

// Opens (and creates if needed) the history database. Pass nullptr to use
// ~/.local/share/kite/kite.db. Returns false when SQLite is unavailable.
bool history_init(const gchar *database_path = nullptr);
void history_close();
bool history_is_ready();

void history_record_visit(const gchar *url, const gchar *title);
void history_update_title(const gchar *url, const gchar *title);

std::vector<HistoryEntry> history_recent(int limit);
std::vector<HistoryEntry> history_search(const gchar *query, int limit);
std::vector<HistoryEntry> history_completions(const gchar *text, int limit);
void history_import_entry(const gchar *url, const gchar *title, gint64 last_visit, int visit_count);
void history_delete_url(const gchar *url);
void history_clear();

bool bookmark_add(const gchar *url, const gchar *title);
bool bookmark_remove(const gchar *url);
bool bookmark_toggle(const gchar *url, const gchar *title, bool *is_bookmarked);
bool bookmark_exists(const gchar *url);
std::vector<HistoryEntry> bookmarks_list();

std::vector<PinnedLink> pinned_links_list();
int pinned_links_count();
bool pinned_link_add(const gchar *url, const gchar *title);
bool pinned_link_remove(const gchar *url);
bool pinned_link_exists(const gchar *url);
