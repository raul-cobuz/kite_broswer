#include "sync.h"

#include <glib/gstdio.h>
#include <jsc/jsc.h>

#include <cerrno>
#include <string>
#include <vector>

#include "history.h"
#include "settings.h"

namespace {

constexpr const char *kSyncFile = "kite-sync.json";

std::string json_string(JSCValue *object, const char *property) {
    if (!object)
        return "";
    JSCValue *value = jsc_value_object_get_property(object, property);
    std::string result;
    if (value && jsc_value_is_string(value)) {
        gchar *text = jsc_value_to_string(value);
        result = text ? text : "";
        g_free(text);
    }
    if (value)
        g_object_unref(value);
    return result;
}

bool json_number(JSCValue *object, const char *property, double *result) {
    if (!object)
        return false;
    JSCValue *value = jsc_value_object_get_property(object, property);
    bool ok = value && jsc_value_is_number(value);
    if (ok)
        *result = jsc_value_to_double(value);
    if (value)
        g_object_unref(value);
    return ok;
}

bool json_bool(JSCValue *object, const char *property, bool *result) {
    if (!object)
        return false;
    JSCValue *value = jsc_value_object_get_property(object, property);
    bool ok = value && jsc_value_is_boolean(value);
    if (ok)
        *result = jsc_value_to_boolean(value) != FALSE;
    if (value)
        g_object_unref(value);
    return ok;
}

void append_escaped(GString *out, const std::string &text) {
    g_string_append_c(out, '"');
    for (unsigned char c : text) {
        switch (c) {
        case '"':
            g_string_append(out, "\\\"");
            break;
        case '\\':
            g_string_append(out, "\\\\");
            break;
        case '\b':
            g_string_append(out, "\\b");
            break;
        case '\f':
            g_string_append(out, "\\f");
            break;
        case '\n':
            g_string_append(out, "\\n");
            break;
        case '\r':
            g_string_append(out, "\\r");
            break;
        case '\t':
            g_string_append(out, "\\t");
            break;
        default:
            if (c < 0x20)
                g_string_append_printf(out, "\\u%04x", c);
            else
                g_string_append_c(out, (gchar)c);
            break;
        }
    }
    g_string_append_c(out, '"');
}

void append_string_field(GString *out, const char *name, const std::string &value) {
    g_string_append_c(out, '"');
    g_string_append(out, name);
    g_string_append(out, "\":");
    append_escaped(out, value);
}

void append_bool_field(GString *out, const char *name, bool value) {
    g_string_append_printf(out, "\"%s\":%s", name, value ? "true" : "false");
}

// Returns the settings object from an existing sync file, or "" when the file
// is missing, unreadable or has no usable settings block.
std::string existing_settings_json(const gchar *path) {
    if (!path || !*path)
        return "";
    gchar *contents = nullptr;
    if (!g_file_get_contents(path, &contents, nullptr, nullptr))
        return "";
    JSCContext *context = jsc_context_new();
    JSCValue *root = jsc_value_new_from_json(context, contents);
    g_free(contents);
    std::string result;
    if (root && jsc_value_is_object(root)) {
        JSCValue *settings = jsc_value_object_get_property(root, "settings");
        if (settings && jsc_value_is_object(settings)) {
            gchar *json = jsc_value_to_json(settings, 0);
            if (json) {
                result = json;
                g_free(json);
            }
        }
        if (settings)
            g_object_unref(settings);
    }
    if (root)
        g_object_unref(root);
    g_object_unref(context);
    return result;
}

} // namespace

const gchar *sync_file_name() {
    return kSyncFile;
}

bool sync_write_file(const gchar *path, bool include_settings, std::string *error) {
    if (!path || !*path) {
        if (error)
            *error = "No sync file path was provided.";
        return false;
    }

    GString *out = g_string_new("{");
    g_string_append_printf(out, "\"format\":1,\"exported_at\":%" G_GINT64_FORMAT ",", g_get_real_time());

    g_string_append(out, "\"bookmarks\":[");
    std::vector<HistoryEntry> bookmarks = bookmarks_list();
    for (size_t i = 0; i < bookmarks.size(); i++) {
        if (i)
            g_string_append_c(out, ',');
        g_string_append_c(out, '{');
        append_string_field(out, "url", bookmarks[i].url);
        g_string_append_c(out, ',');
        append_string_field(out, "title", bookmarks[i].title);
        g_string_append_printf(out, ",\"created\":%" G_GINT64_FORMAT, bookmarks[i].last_visit);
        g_string_append_c(out, '}');
    }
    g_string_append(out, "],");

    g_string_append(out, "\"pins\":[");
    std::vector<PinnedLink> pins = pinned_links_list();
    for (size_t i = 0; i < pins.size(); i++) {
        if (i)
            g_string_append_c(out, ',');
        g_string_append_c(out, '{');
        append_string_field(out, "url", pins[i].url);
        g_string_append_c(out, ',');
        append_string_field(out, "title", pins[i].title);
        g_string_append_c(out, '}');
    }
    g_string_append(out, "],");

    g_string_append(out, "\"history\":[");
    std::vector<HistoryEntry> history = history_recent(G_MAXINT);
    for (size_t i = 0; i < history.size(); i++) {
        if (i)
            g_string_append_c(out, ',');
        g_string_append_c(out, '{');
        append_string_field(out, "url", history[i].url);
        g_string_append_c(out, ',');
        append_string_field(out, "title", history[i].title);
        g_string_append_printf(out, ",\"last_visit\":%" G_GINT64_FORMAT ",\"visit_count\":%d", history[i].last_visit,
                               history[i].visit_count);
        g_string_append_c(out, '}');
    }
    g_string_append(out, "],");

    g_string_append(out, "\"deleted\":[");
    std::vector<SyncDeletion> deletions = sync_deletions();
    for (size_t i = 0; i < deletions.size(); i++) {
        if (i)
            g_string_append_c(out, ',');
        g_string_append_c(out, '{');
        append_string_field(out, "kind", deletions[i].kind);
        g_string_append_c(out, ',');
        append_string_field(out, "url", deletions[i].url);
        g_string_append_printf(out, ",\"deleted_at\":%" G_GINT64_FORMAT, deletions[i].deleted_at);
        g_string_append_c(out, '}');
    }
    g_string_append(out, "],");

    if (include_settings) {
        g_string_append(out, "\"settings\":{");
        append_string_field(out, "homepage", settings_homepage());
        g_string_append_c(out, ',');
        append_string_field(out, "search_engine", settings_search_engine());
        g_string_append_c(out, ',');
        append_bool_field(out, "google_fallback", settings_google_fallback());
        g_string_append_c(out, ',');
        append_bool_field(out, "dark_mode", settings_dark_mode());
        g_string_append_c(out, ',');
        append_bool_field(out, "restore_session", settings_restore_session());
        g_string_append_c(out, ',');
        append_bool_field(out, "block_ads", settings_block_ads());
        g_string_append_c(out, ',');
        append_bool_field(out, "strip_tracking_params", settings_strip_tracking_params());
        g_string_append_c(out, ',');
        append_bool_field(out, "clear_on_exit", settings_clear_on_exit());
        g_string_append_printf(out, ",\"updated_at\":%" G_GINT64_FORMAT, settings_saved_at());
        g_string_append(out, "}");
    } else {
        std::string preserved = existing_settings_json(path);
        g_string_append(out, "\"settings\":");
        g_string_append(out, preserved.empty() ? "{}" : preserved.c_str());
    }
    g_string_append(out, "}");

    gchar *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);

    GError *write_error = nullptr;
    bool ok = g_file_set_contents(path, out->str, (gssize)out->len, &write_error);
    if (!ok && error)
        *error = write_error ? write_error->message : "Could not write the sync file.";
    g_clear_error(&write_error);
    if (ok)
        g_chmod(path, 0600);
    g_string_free(out, TRUE);
    return ok;
}

bool sync_merge_file(const gchar *path, bool include_settings, SyncResult *result, std::string *error) {
    SyncResult local_result;
    SyncResult &stats = result ? *result : local_result;
    stats = SyncResult();

    if (!path || !*path) {
        if (error)
            *error = "No sync file path was provided.";
        return false;
    }

    gchar *contents = nullptr;
    gsize length = 0;
    GError *read_error = nullptr;
    if (!g_file_get_contents(path, &contents, &length, &read_error)) {
        if (error)
            *error = read_error ? read_error->message : "Could not read the sync file.";
        g_clear_error(&read_error);
        return false;
    }

    JSCContext *context = jsc_context_new();
    JSCValue *root = jsc_value_new_from_json(context, contents);
    g_free(contents);

    if (!root || !jsc_value_is_object(root)) {
        if (error) {
            JSCException *exception = jsc_context_get_exception(context);
            *error = exception ? jsc_exception_get_message(exception) : "Invalid sync JSON.";
            jsc_context_clear_exception(context);
        }
        if (root)
            g_object_unref(root);
        g_object_unref(context);
        return false;
    }

    std::vector<SyncDeletion> file_deletions;
    bool merge_ok = true;
    // shortcut: helpers still prepare a statement per row. The per-row fsync
    // dominates, so one transaction is the useful win; hoist the statements
    // only if profiling says otherwise.
    bool in_transaction = history_transaction_begin();
    JSCValue *deleted = jsc_value_object_get_property(root, "deleted");
    if (deleted && jsc_value_is_array(deleted)) {
        JSCValue *length_value = jsc_value_object_get_property(deleted, "length");
        int count = length_value ? jsc_value_to_int32(length_value) : 0;
        if (length_value)
            g_object_unref(length_value);
        for (int i = 0; i < count; i++) {
            JSCValue *entry = jsc_value_object_get_property_at_index(deleted, (guint)i);
            if (!entry || !jsc_value_is_object(entry)) {
                if (entry)
                    g_object_unref(entry);
                continue;
            }
            SyncDeletion item;
            item.kind = json_string(entry, "kind");
            item.url = json_string(entry, "url");
            double deleted_at = 0;
            json_number(entry, "deleted_at", &deleted_at);
            g_object_unref(entry);
            if (item.kind.empty() || item.url.empty())
                continue;
            item.deleted_at = (gint64)deleted_at;
            file_deletions.push_back(item);
            sync_apply_deletion(item.kind.c_str(), item.url.c_str(), item.deleted_at);
        }
    }
    if (deleted)
        g_object_unref(deleted);

    auto file_declares_deletion = [&file_deletions](const char *kind, const char *url) {
        for (const auto &item : file_deletions) {
            if (item.kind == kind && item.url == url)
                return true;
        }
        return false;
    };

    JSCValue *bookmarks = jsc_value_object_get_property(root, "bookmarks");
    if (bookmarks && jsc_value_is_array(bookmarks)) {
        JSCValue *length_value = jsc_value_object_get_property(bookmarks, "length");
        int count = length_value ? jsc_value_to_int32(length_value) : 0;
        if (length_value)
            g_object_unref(length_value);
        for (int i = 0; i < count; i++) {
            JSCValue *entry = jsc_value_object_get_property_at_index(bookmarks, (guint)i);
            if (!entry || !jsc_value_is_object(entry)) {
                if (entry)
                    g_object_unref(entry);
                continue;
            }
            std::string url = json_string(entry, "url");
            std::string title = json_string(entry, "title");
            double created = 0;
            json_number(entry, "created", &created);
            g_object_unref(entry);
            if (url.empty())
                continue;
            gint64 deleted_at = 0;
            if (sync_get_deletion("bookmark", url.c_str(), &deleted_at)) {
                // Keep the deletion unless this bookmark was created after it.
                if (created <= 0 || (gint64)created <= deleted_at)
                    continue;
                sync_clear_deletion("bookmark", url.c_str());
            }
            bool existed = bookmark_exists(url.c_str());
            if (!bookmark_add(url.c_str(), title.c_str())) {
                merge_ok = false;
            } else if (!existed) {
                stats.bookmarks_added++;
            }
        }
    }
    if (bookmarks)
        g_object_unref(bookmarks);

    JSCValue *pins = jsc_value_object_get_property(root, "pins");
    if (pins && jsc_value_is_array(pins)) {
        JSCValue *length_value = jsc_value_object_get_property(pins, "length");
        int count = length_value ? jsc_value_to_int32(length_value) : 0;
        if (length_value)
            g_object_unref(length_value);
        for (int i = 0; i < count; i++) {
            JSCValue *entry = jsc_value_object_get_property_at_index(pins, (guint)i);
            if (!entry || !jsc_value_is_object(entry)) {
                if (entry)
                    g_object_unref(entry);
                continue;
            }
            std::string url = json_string(entry, "url");
            std::string title = json_string(entry, "title");
            g_object_unref(entry);
            if (url.empty() || pinned_link_exists(url.c_str()))
                continue;
            gint64 deleted_at = 0;
            if (sync_get_deletion("pin", url.c_str(), &deleted_at)) {
                if (file_declares_deletion("pin", url.c_str()))
                    continue;
                sync_clear_deletion("pin", url.c_str());
            }
            if (pinned_link_add(url.c_str(), title.c_str()))
                stats.pins_added++;
        }
    }
    if (pins)
        g_object_unref(pins);

    JSCValue *history = jsc_value_object_get_property(root, "history");
    if (history && jsc_value_is_array(history)) {
        JSCValue *length_value = jsc_value_object_get_property(history, "length");
        int count = length_value ? jsc_value_to_int32(length_value) : 0;
        if (length_value)
            g_object_unref(length_value);
        for (int i = 0; i < count; i++) {
            JSCValue *entry = jsc_value_object_get_property_at_index(history, (guint)i);
            if (!entry || !jsc_value_is_object(entry)) {
                if (entry)
                    g_object_unref(entry);
                continue;
            }
            std::string url = json_string(entry, "url");
            std::string title = json_string(entry, "title");
            double last_visit = 0;
            double visit_count = 0;
            json_number(entry, "last_visit", &last_visit);
            json_number(entry, "visit_count", &visit_count);
            g_object_unref(entry);
            if (url.empty())
                continue;
            gint64 deleted_at = 0;
            gint64 wildcard_deleted_at = 0;
            bool hidden = sync_get_deletion("history", url.c_str(), &deleted_at);
            bool all_hidden = sync_get_deletion("history", "*", &wildcard_deleted_at);
            gint64 cutoff = deleted_at > wildcard_deleted_at ? deleted_at : wildcard_deleted_at;
            if ((hidden || all_hidden) && (gint64)last_visit <= cutoff)
                continue;
            history_import_entry(url.c_str(), title.c_str(), (gint64)last_visit, (int)visit_count);
            stats.history_added++;
        }
    }
    if (history)
        g_object_unref(history);

    if (in_transaction) {
        if (merge_ok) {
            merge_ok = history_transaction_commit();
            if (!merge_ok)
                history_transaction_rollback();
        } else {
            history_transaction_rollback();
        }
    }
    if (!merge_ok) {
        if (error)
            *error = "Could not merge the sync file.";
        g_object_unref(root);
        g_object_unref(context);
        return false;
    }

    if (include_settings) {
        JSCValue *settings = jsc_value_object_get_property(root, "settings");
        if (settings && jsc_value_is_object(settings)) {
            double updated_at = 0;
            bool has_updated_at = json_number(settings, "updated_at", &updated_at);
            bool stale = has_updated_at && updated_at > 0 && (gint64)updated_at <= settings_saved_at();
            if (!stale) {
                std::string homepage = json_string(settings, "homepage");
                bool value = false;
                if (!homepage.empty()) {
                    settings_set_homepage(homepage.c_str());
                    stats.settings_applied = true;
                }
                std::string search_engine = json_string(settings, "search_engine");
                if (!search_engine.empty()) {
                    settings_set_search_engine(search_engine.c_str());
                    stats.settings_applied = true;
                }
                if (json_bool(settings, "google_fallback", &value)) {
                    settings_set_google_fallback(value);
                    stats.settings_applied = true;
                }
                if (json_bool(settings, "dark_mode", &value)) {
                    settings_set_dark_mode(value);
                    stats.settings_applied = true;
                }
                if (json_bool(settings, "restore_session", &value)) {
                    settings_set_restore_session(value);
                    stats.settings_applied = true;
                }
                if (json_bool(settings, "block_ads", &value)) {
                    settings_set_block_ads(value);
                    stats.settings_applied = true;
                }
                if (json_bool(settings, "strip_tracking_params", &value)) {
                    settings_set_strip_tracking_params(value);
                    stats.settings_applied = true;
                }
                if (json_bool(settings, "clear_on_exit", &value)) {
                    settings_set_clear_on_exit(value);
                    stats.settings_applied = true;
                }
            }
        }
        if (settings)
            g_object_unref(settings);
    }

    g_object_unref(root);
    g_object_unref(context);

    if (stats.settings_applied)
        save_settings();
    return true;
}

bool sync_run(const gchar *folder, bool include_settings, SyncResult *result, std::string *error) {
    if (!folder || !*folder) {
        if (error)
            *error = "Choose a sync folder first.";
        return false;
    }

    if (g_mkdir_with_parents(folder, 0700) != 0) {
        if (error)
            *error = g_strerror(errno);
        return false;
    }

    gchar *path = g_build_filename(folder, kSyncFile, nullptr);
    SyncResult local_result;
    SyncResult &stats = result ? *result : local_result;
    stats = SyncResult();

    if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
        if (!sync_merge_file(path, include_settings, &stats, error)) {
            g_free(path);
            return false;
        }
    }

    bool ok = sync_write_file(path, include_settings, error);
    g_free(path);
    return ok;
}
