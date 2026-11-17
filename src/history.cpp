#include "history.h"

#include <glib/gstdio.h>
#include <sqlite3.h>

#include <cstring>
#include <unordered_set>

#include "url_utils.h"
#include "profile.h"

static sqlite3 *db = nullptr;
static gchar *db_path = nullptr;

static int capped_limit(int limit) {
    if (limit <= 0)
        return 50;
    return limit;
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

static bool table_has_column(const char *table, const char *column) {
    if (!db)
        return false;
    gchar *sql = g_strdup_printf("PRAGMA table_info(%s)", table);
    sqlite3_stmt *stmt = nullptr;
    bool found = false;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *name = sqlite3_column_text(stmt, 1);
            if (name && g_strcmp0((const char *)name, column) == 0) {
                found = true;
                break;
            }
        }
        sqlite3_finalize(stmt);
    }
    g_free(sql);
    return found;
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
                  "visit_count INTEGER NOT NULL DEFAULT 1,"
                  "search_text TEXT NOT NULL DEFAULT ''"
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
                  ") WITHOUT ROWID;") ||
        !exec_sql("CREATE TABLE IF NOT EXISTS sync_deletions("
                  "kind TEXT NOT NULL,"
                  "url TEXT NOT NULL,"
                  "deleted_at INTEGER NOT NULL,"
                  "PRIMARY KEY(kind,url)"
                  ") WITHOUT ROWID;")) {
        history_close();
        return false;
    }

    // Existing databases predate search_text; backfill it once, then let the
    // triggers keep it current without touching every write path.
    if (!table_has_column("history", "search_text")) {
        if (!exec_sql("ALTER TABLE history ADD COLUMN search_text TEXT NOT NULL DEFAULT '';") ||
            !exec_sql("UPDATE history SET search_text=lower(url || ' ' || title);")) {
            history_close();
            return false;
        }
    }
    if (!exec_sql("CREATE TRIGGER IF NOT EXISTS history_search_insert AFTER INSERT ON history BEGIN "
                  "UPDATE history SET search_text=lower(new.url || ' ' || new.title) WHERE url=new.url;"
                  "END;") ||
        !exec_sql("CREATE TRIGGER IF NOT EXISTS history_search_update AFTER UPDATE OF url,title ON history BEGIN "
                  "UPDATE history SET search_text=lower(new.url || ' ' || new.title) WHERE url=new.url;"
                  "END;")) {
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

bool history_transaction_begin() {
    return exec_sql("BEGIN IMMEDIATE;");
}

bool history_transaction_commit() {
    return exec_sql("COMMIT;");
}

void history_transaction_rollback() {
    exec_sql("ROLLBACK;");
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
    else
        sync_clear_deletion("history", url);
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

// shortcut: full scan over the pre-lowered search_text column. Fine for tens of
// thousands of rows; replace with an FTS5 index if libraries grow beyond that.
std::vector<HistoryEntry> history_completions(const gchar *text, int limit) {
    std::vector<HistoryEntry> matches;
    if (!db || !text || !*text)
        return matches;

    int max_entries = capped_limit(limit);
    std::unordered_set<std::string> seen;

    // search_text is kept lowercased by triggers, so the query avoids two
    // lower() calls per row. It is still a scan; see the shortcut note below.
    sqlite3_stmt *stmt = nullptr;
    const char *history_sql =
        "SELECT url,title,host,last_visit,visit_count FROM history "
        "WHERE instr(search_text, lower(?1))>0 "
        "ORDER BY CASE WHEN instr(search_text, lower(?1))=1 THEN 0 ELSE 1 END,"
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
    gint64 now = g_get_real_time();
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, "DELETE FROM history WHERE url=?1", -1, &stmt, nullptr) != SQLITE_OK)
        return;
    sqlite3_bind_text(stmt, 1, url, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (sqlite3_changes(db) > 0)
        sync_record_deletion("history", url, now);
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
    gint64 now = g_get_real_time();
    exec_sql("DELETE FROM history;");
    if (db && sqlite3_changes(db) > 0)
        sync_record_deletion("history", "*", now);
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
    if (ok)
        sync_clear_deletion("bookmark", url);
    return ok;
}

bool bookmark_remove(const gchar *url) {
    if (!db || !url || !*url)
        return false;
    gint64 now = g_get_real_time();
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, "DELETE FROM bookmarks WHERE url=?1", -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, url, -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (ok && sqlite3_changes(db) > 0)
        sync_record_deletion("bookmark", url, now);
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
    const char *sql = "SELECT url,title,'',created,0 FROM bookmarks ORDER BY created DESC";
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
        if (ok)
            sync_clear_deletion("pin", url);
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
    if (ok)
        sync_clear_deletion("pin", url);
    return ok;
}

bool pinned_link_remove(const gchar *url) {
    if (!db || !url || !*url)
        return false;

    gint64 now = g_get_real_time();
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, "DELETE FROM pinned_links WHERE url=?1", -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, url, -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (ok && sqlite3_changes(db) > 0)
        sync_record_deletion("pin", url, now);
    return ok;
}

// ---------------------------------------------------------------------------
// Sync tombstones

static bool query_timestamp(const char *sql, const gchar *url, gint64 *value) {
    if (!db || !url || !*url)
        return false;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, url, -1, SQLITE_TRANSIENT);
    bool found = sqlite3_step(stmt) == SQLITE_ROW;
    if (found && value)
        *value = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return found;
}

gint64 bookmark_created_at(const gchar *url) {
    gint64 created = 0;
    if (!query_timestamp("SELECT created FROM bookmarks WHERE url=?1", url, &created))
        return 0;
    return created;
}

gint64 history_last_visit(const gchar *url) {
    gint64 last_visit = 0;
    if (!query_timestamp("SELECT last_visit FROM history WHERE url=?1", url, &last_visit))
        return 0;
    return last_visit;
}

void sync_record_deletion(const gchar *kind, const gchar *url, gint64 deleted_at) {
    if (!db || !kind || !*kind || !url || !*url)
        return;
    if (deleted_at <= 0)
        deleted_at = g_get_real_time();

    sqlite3_stmt *stmt = nullptr;
    const char *sql =
        "INSERT INTO sync_deletions(kind,url,deleted_at) VALUES(?1,?2,?3) "
        "ON CONFLICT(kind,url) DO UPDATE SET deleted_at=MAX(sync_deletions.deleted_at,excluded.deleted_at)";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return;
    sqlite3_bind_text(stmt, 1, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, url, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, deleted_at);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void sync_clear_deletion(const gchar *kind, const gchar *url) {
    if (!db || !kind || !*kind || !url || !*url)
        return;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, "DELETE FROM sync_deletions WHERE kind=?1 AND url=?2", -1, &stmt, nullptr) !=
        SQLITE_OK)
        return;
    sqlite3_bind_text(stmt, 1, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, url, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

bool sync_get_deletion(const gchar *kind, const gchar *url, gint64 *deleted_at) {
    if (!db || !kind || !*kind || !url || !*url)
        return false;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT deleted_at FROM sync_deletions WHERE kind=?1 AND url=?2", -1, &stmt,
                           nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, url, -1, SQLITE_TRANSIENT);
    bool found = sqlite3_step(stmt) == SQLITE_ROW;
    if (found && deleted_at)
        *deleted_at = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return found;
}

std::vector<SyncDeletion> sync_deletions() {
    std::vector<SyncDeletion> entries;
    if (!db)
        return entries;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT kind,url,deleted_at FROM sync_deletions ORDER BY deleted_at", -1, &stmt,
                           nullptr) != SQLITE_OK)
        return entries;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *kind = sqlite3_column_text(stmt, 0);
        const unsigned char *url = sqlite3_column_text(stmt, 1);
        SyncDeletion entry;
        entry.kind = kind ? (const char *)kind : "";
        entry.url = url ? (const char *)url : "";
        entry.deleted_at = sqlite3_column_int64(stmt, 2);
        if (!entry.kind.empty() && !entry.url.empty())
            entries.push_back(entry);
    }
    sqlite3_finalize(stmt);
    return entries;
}

static void delete_matching(const char *sql, const gchar *url) {
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return;
    sqlite3_bind_text(stmt, 1, url, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void sync_apply_deletion(const gchar *kind, const gchar *url, gint64 deleted_at) {
    if (!db || !kind || !*kind || !url || !*url)
        return;
    if (deleted_at <= 0)
        deleted_at = g_get_real_time();

    sync_record_deletion(kind, url, deleted_at);

    if (g_strcmp0(kind, "bookmark") == 0) {
        gint64 created = bookmark_created_at(url);
        if (created > 0 && created > deleted_at)
            return;
        delete_matching("DELETE FROM bookmarks WHERE url=?1", url);
    } else if (g_strcmp0(kind, "pin") == 0) {
        delete_matching("DELETE FROM pinned_links WHERE url=?1", url);
    } else if (g_strcmp0(kind, "history") == 0) {
        if (g_strcmp0(url, "*") == 0) {
            sqlite3_stmt *stmt = nullptr;
            if (sqlite3_prepare_v2(db, "DELETE FROM history WHERE last_visit<=?1", -1, &stmt, nullptr) !=
                SQLITE_OK)
                return;
            sqlite3_bind_int64(stmt, 1, deleted_at);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            return;
        }
        gint64 last_visit = history_last_visit(url);
        if (last_visit > 0 && last_visit > deleted_at)
            return;
        delete_matching("DELETE FROM history WHERE url=?1", url);
    }
}
