#include "history.h"

#include <glib/gstdio.h>
#include <sqlite3.h>

#include <algorithm>
#include <cstring>
#include <unordered_set>

#include "url_utils.h"
#include "profile.h"

static sqlite3 *db = nullptr;
static gchar *db_path = nullptr;

static int capped_limit(int limit) {
    if (limit <= 0)
        return 50;
    return std::min(limit, 1000);
}

static bool exec_sql(const char *sql) {
    if (!db)
        return false;
    char *error = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        g_printerr("[Kite] History SQL error: %s\n", error ? error : sqlite3_errmsg(db));
        sqlite3_free(error);
        return false;
    }
    return true;
}

static HistoryEntry read_entry(sqlite3_stmt *stmt) {
    HistoryEntry entry;
    const unsigned char *url = sqlite3_column_text(stmt, 0);
    const unsigned char *title = sqlite3_column_text(stmt, 1);
    const unsigned char *host = sqlite3_column_text(stmt, 2);
    entry.url = url ? (const char *)url : "";
    entry.title = title ? (const char *)title : "";
    entry.host = host ? (const char *)host : "";
    entry.last_visit = sqlite3_column_int64(stmt, 3);
    entry.visit_count = sqlite3_column_int(stmt, 4);
    return entry;
}

bool history_init(const gchar *database_path) {
    history_close();

    if (database_path && *database_path)
        db_path = g_strdup(database_path);
    else
        db_path = g_build_filename(profile_data_dir(), "kite.db", nullptr);

    gchar *dir = g_path_get_dirname(db_path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);

    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        g_printerr("[Kite] Could not open history database %s: %s\n", db_path, db ? sqlite3_errmsg(db) : "unknown error");
        if (db) {
            sqlite3_close(db);
            db = nullptr;
        }
        g_free(db_path);
        db_path = nullptr;
        return false;
    }
    g_chmod(db_path, 0600);
    sqlite3_busy_timeout(db, 2000);

    if (!exec_sql("PRAGMA journal_mode=DELETE;") || !exec_sql("PRAGMA synchronous=NORMAL;") ||
        !exec_sql("CREATE TABLE IF NOT EXISTS history("
                  "url TEXT PRIMARY KEY,"
                  "title TEXT NOT NULL DEFAULT '',"
                  "host TEXT NOT NULL DEFAULT '',"
                  "last_visit INTEGER NOT NULL,"
                  "visit_count INTEGER NOT NULL DEFAULT 1"
                  ") WITHOUT ROWID;") ||
        !exec_sql("CREATE INDEX IF NOT EXISTS history_last_visit ON history(last_visit DESC);") ||
        !exec_sql("CREATE TABLE IF NOT EXISTS bookmarks("
                  "url TEXT PRIMARY KEY,"
                  "title TEXT NOT NULL DEFAULT '',"
                  "created INTEGER NOT NULL"
                  ") WITHOUT ROWID;") ||
        !exec_sql("CREATE INDEX IF NOT EXISTS bookmarks_created ON bookmarks(created DESC);") ||
        !exec_sql("CREATE TABLE IF NOT EXISTS pinned_links("
                  "url TEXT PRIMARY KEY,"
                  "title TEXT NOT NULL DEFAULT '',"
                  "position INTEGER NOT NULL"
                  ") WITHOUT ROWID;")) {
        history_close();
        return false;
    }
    return true;
}

void history_close() {
    if (db) {
        sqlite3_close(db);
        db = nullptr;
    }
    g_free(db_path);
    db_path = nullptr;
}

bool history_is_ready() {
    return db != nullptr;
}

void history_record_visit(const gchar *url, const gchar *title) {
    if (!db || !url || !*url)
        return;
    if (g_str_has_prefix(url, "kite:") || g_str_has_prefix(url, "about:"))
        return;

    static const char *sql =
        "INSERT INTO history(url,title,host,last_visit,visit_count) VALUES(?1,?2,?3,?4,1) "
        "ON CONFLICT(url) DO UPDATE SET "
        "title=CASE WHEN excluded.title<>'' THEN excluded.title ELSE history.title END,"
        "host=excluded.host,last_visit=excluded.last_visit,visit_count=history.visit_count+1";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        g_printerr("[Kite] Could not record history: %s\n", sqlite3_errmsg(db));
        return;
    }

    std::string host = url_host(url);
    sqlite3_bind_text(stmt, 1, url, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, title ? title : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, host.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, g_get_real_time());
    if (sqlite3_step(stmt) != SQLITE_DONE)
        g_printerr("[Kite] Could not record history: %s\n", sqlite3_errmsg(db));
    sqlite3_finalize(stmt);
}

void history_update_title(const gchar *url, const gchar *title) {
    if (!db || !url || !*url || !title || !*title)
        return;

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, "UPDATE history SET title=?2 WHERE url=?1", -1, &stmt, nullptr) != SQLITE_OK)
        return;
    sqlite3_bind_text(stmt, 1, url, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, title, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::vector<HistoryEntry> history_recent(int limit) {
    std::vector<HistoryEntry> entries;
    if (!db)
        return entries;

    sqlite3_stmt *stmt = nullptr;
    const char *sql = "SELECT url,title,host,last_visit,visit_count FROM history "
                      "ORDER BY last_visit DESC LIMIT ?1";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return entries;

    sqlite3_bind_int(stmt, 1, capped_limit(limit));
    while (sqlite3_step(stmt) == SQLITE_ROW)
        entries.push_back(read_entry(stmt));
    sqlite3_finalize(stmt);
    return entries;
}

std::vector<HistoryEntry> history_search(const gchar *query, int limit) {
    if (!query || !*query)
        return history_recent(limit);

    std::vector<HistoryEntry> entries;
    if (!db)
        return entries;

    sqlite3_stmt *stmt = nullptr;
    const char *sql =
        "SELECT url,title,host,last_visit,visit_count FROM history "
        "WHERE instr(lower(url), lower(?1))>0 OR instr(lower(title), lower(?1))>0 "
        "ORDER BY CASE WHEN instr(lower(url), lower(?1))=1 THEN 0 ELSE 1 END,"
        "visit_count DESC, last_visit DESC LIMIT ?2";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return entries;

    sqlite3_bind_text(stmt, 1, query, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, capped_limit(limit));
    while (sqlite3_step(stmt) == SQLITE_ROW)
        entries.push_back(read_entry(stmt));
    sqlite3_finalize(stmt);
    return entries;
}

std::vector<HistoryEntry> history_completions(const gchar *text, int limit) {
    std::vector<HistoryEntry> matches;
    if (!db || !text || !*text)
        return matches;

    int max_entries = capped_limit(limit);
    std::unordered_set<std::string> seen;

    sqlite3_stmt *stmt = nullptr;
    const char *history_sql =
        "SELECT url,title,host,last_visit,visit_count FROM history "
        "WHERE instr(lower(url), lower(?1))>0 OR instr(lower(title), lower(?1))>0 "
        "ORDER BY CASE WHEN instr(lower(url), lower(?1))=1 THEN 0 ELSE 1 END,"
        "visit_count DESC, last_visit DESC LIMIT ?2";
    if (sqlite3_prepare_v2(db, history_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, text, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, max_entries);
        while ((int)matches.size() < max_entries && sqlite3_step(stmt) == SQLITE_ROW) {
            HistoryEntry entry = read_entry(stmt);
            if (seen.insert(entry.url).second)
                matches.push_back(entry);
        }
        sqlite3_finalize(stmt);
    }

    const char *bookmark_sql =
        "SELECT url,title,'',created,0 FROM bookmarks "
        "WHERE instr(lower(url), lower(?1))>0 OR instr(lower(title), lower(?1))>0 "
        "ORDER BY CASE WHEN instr(lower(url), lower(?1))=1 THEN 0 ELSE 1 END, created DESC LIMIT ?2";
    if ((int)matches.size() < max_entries && sqlite3_prepare_v2(db, bookmark_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, text, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, max_entries);
        while ((int)matches.size() < max_entries && sqlite3_step(stmt) == SQLITE_ROW) {
            HistoryEntry entry = read_entry(stmt);
            entry.host = url_host(entry.url.c_str());
            if (seen.insert(entry.url).second)
                matches.push_back(entry);
        }
        sqlite3_finalize(stmt);
    }

    return matches;
}

void history_delete_url(const gchar *url) {
    if (!db || !url || !*url)
        return;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, "DELETE FROM history WHERE url=?1", -1, &stmt, nullptr) != SQLITE_OK)
        return;
    sqlite3_bind_text(stmt, 1, url, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void history_import_entry(const gchar *url, const gchar *title, gint64 last_visit, int visit_count) {
    if (!db || !url || !*url)
        return;
    if (g_str_has_prefix(url, "kite:") || g_str_has_prefix(url, "about:"))
        return;

    static const char *sql =
        "INSERT INTO history(url,title,host,last_visit,visit_count) VALUES(?1,?2,?3,?4,?5) "
        "ON CONFLICT(url) DO UPDATE SET "
        "title=CASE WHEN excluded.title<>'' THEN excluded.title ELSE history.title END,"
        "host=excluded.host,"
        "last_visit=MAX(history.last_visit,excluded.last_visit),"
        "visit_count=MAX(history.visit_count,excluded.visit_count)";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return;

    std::string host = url_host(url);
    sqlite3_bind_text(stmt, 1, url, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, title ? title : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, host.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, last_visit > 0 ? last_visit : g_get_real_time());
    sqlite3_bind_int(stmt, 5, visit_count > 0 ? visit_count : 1);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void history_clear() {
    exec_sql("DELETE FROM history;");
}

bool bookmark_add(const gchar *url, const gchar *title) {
    if (!db || !url || !*url)
        return false;

    static const char *sql =
        "INSERT INTO bookmarks(url,title,created) VALUES(?1,?2,?3) "
        "ON CONFLICT(url) DO UPDATE SET title=CASE WHEN excluded.title<>'' THEN excluded.title ELSE bookmarks.title END";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        g_printerr("[Kite] Could not add bookmark: %s\n", sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_text(stmt, 1, url, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, title ? title : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, g_get_real_time());
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool bookmark_remove(const gchar *url) {
    if (!db || !url || !*url)
        return false;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, "DELETE FROM bookmarks WHERE url=?1", -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, url, -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool bookmark_toggle(const gchar *url, const gchar *title, bool *is_bookmarked) {
    if (!db || !url || !*url)
        return false;
    bool exists = bookmark_exists(url);
    bool ok = exists ? bookmark_remove(url) : bookmark_add(url, title);
    if (ok && is_bookmarked)
        *is_bookmarked = !exists;
    return ok;
}

bool bookmark_exists(const gchar *url) {
    if (!db || !url || !*url)
        return false;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM bookmarks WHERE url=?1 LIMIT 1", -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, url, -1, SQLITE_TRANSIENT);
    bool exists = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return exists;
}

std::vector<HistoryEntry> bookmarks_list() {
    std::vector<HistoryEntry> entries;
    if (!db)
        return entries;

    sqlite3_stmt *stmt = nullptr;
    const char *sql = "SELECT url,title,'',created,0 FROM bookmarks ORDER BY created DESC LIMIT 1000";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return entries;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        HistoryEntry entry = read_entry(stmt);
        entry.host = url_host(entry.url.c_str());
        entries.push_back(entry);
    }
    sqlite3_finalize(stmt);
    return entries;
}

std::vector<PinnedLink> pinned_links_list() {
    std::vector<PinnedLink> links;
    if (!db)
        return links;

    sqlite3_stmt *stmt = nullptr;
    const char *sql = "SELECT url,title FROM pinned_links ORDER BY position ASC, url ASC LIMIT ?1";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return links;
    sqlite3_bind_int(stmt, 1, KITE_MAX_PINNED_LINKS);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *url = sqlite3_column_text(stmt, 0);
        const unsigned char *title = sqlite3_column_text(stmt, 1);
        PinnedLink link;
        link.url = url ? (const char *)url : "";
        link.title = title ? (const char *)title : "";
        if (!link.url.empty())
            links.push_back(link);
    }
    sqlite3_finalize(stmt);
    return links;
}

int pinned_links_count() {
    if (!db)
        return 0;

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM pinned_links", -1, &stmt, nullptr) != SQLITE_OK)
        return 0;
    int count = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : 0;
    sqlite3_finalize(stmt);
    return count;
}

bool pinned_link_exists(const gchar *url) {
    if (!db || !url || !*url)
        return false;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM pinned_links WHERE url=?1 LIMIT 1", -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, url, -1, SQLITE_TRANSIENT);
    bool exists = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return exists;
}

bool pinned_link_add(const gchar *url, const gchar *title) {
    if (!db || !url || !*url)
        return false;

    if (pinned_link_exists(url)) {
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db, "UPDATE pinned_links SET title=?2 WHERE url=?1", -1, &stmt, nullptr) !=
            SQLITE_OK)
            return false;
        sqlite3_bind_text(stmt, 1, url, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, title ? title : "", -1, SQLITE_TRANSIENT);
        bool ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        return ok;
    }

    if (pinned_links_count() >= KITE_MAX_PINNED_LINKS)
        return false;

    sqlite3_stmt *stmt = nullptr;
    const char *sql =
        "INSERT INTO pinned_links(url,title,position) VALUES(?1,?2,"
        "(SELECT COALESCE(MAX(position),-1)+1 FROM pinned_links))";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, url, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, title ? title : "", -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool pinned_link_remove(const gchar *url) {
    if (!db || !url || !*url)
        return false;

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, "DELETE FROM pinned_links WHERE url=?1", -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, url, -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}
