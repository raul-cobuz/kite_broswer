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

} // namespace

const gchar *sync_file_name() {
    return kSyncFile;
}

bool sync_write_file(const gchar *path, std::string *error) {
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
    std::vector<HistoryEntry> history = history_recent(1000);
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
    g_string_append(out, "}}");

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
            g_object_unref(entry);
            if (url.empty())
                continue;
            bool existed = bookmark_exists(url.c_str());
            if (bookmark_add(url.c_str(), title.c_str()) && !existed)
                stats.bookmarks_added++;
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
            history_import_entry(url.c_str(), title.c_str(), (gint64)last_visit, (int)visit_count);
            stats.history_added++;
        }
    }
    if (history)
        g_object_unref(history);

    if (include_settings) {
        JSCValue *settings = jsc_value_object_get_property(root, "settings");
        if (settings && jsc_value_is_object(settings)) {
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

    bool ok = sync_write_file(path, error);
    g_free(path);
    return ok;
}
