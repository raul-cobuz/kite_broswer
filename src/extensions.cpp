#include "extensions.h"

#include <glib/gstdio.h>
#include <jsc/jsc.h>
#include <webkit2/webkit2.h>

#include <algorithm>
#include <cerrno>
#include <string>
#include <vector>

#include "adblock.h"

namespace {

struct ContentScriptSpec {
    std::vector<std::string> matches;
    std::vector<std::string> js;
    std::vector<std::string> css;
    std::string run_at;
    bool all_frames = false;
};

gchar *extension_root = nullptr;
std::vector<WebKitUserScript *> injected_scripts;
std::vector<WebKitUserStyleSheet *> injected_styles;
std::vector<ExtensionInfo> loaded_extensions;

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

bool json_bool(JSCValue *object, const char *property) {
    if (!object)
        return false;
    JSCValue *value = jsc_value_object_get_property(object, property);
    bool result = value && jsc_value_is_boolean(value) && jsc_value_to_boolean(value);
    if (value)
        g_object_unref(value);
    return result;
}

std::vector<std::string> json_string_array(JSCValue *object, const char *property) {
    std::vector<std::string> result;
    if (!object)
        return result;

    JSCValue *array = jsc_value_object_get_property(object, property);
    if (!array || !jsc_value_is_array(array)) {
        if (array)
            g_object_unref(array);
        return result;
    }

    JSCValue *length_value = jsc_value_object_get_property(array, "length");
    int length = length_value ? jsc_value_to_int32(length_value) : 0;
    if (length_value)
        g_object_unref(length_value);

    for (int i = 0; i < length; i++) {
        JSCValue *item = jsc_value_object_get_property_at_index(array, (guint)i);
        if (item && jsc_value_is_string(item)) {
            gchar *text = jsc_value_to_string(item);
            if (text && *text)
                result.push_back(text);
            g_free(text);
        }
        if (item)
            g_object_unref(item);
    }
    g_object_unref(array);
    return result;
}

std::string json_error(JSCContext *context) {
    JSCException *exception = jsc_context_get_exception(context);
    if (!exception)
        return "invalid JSON";
    const char *message = jsc_exception_get_message(exception);
    std::string result = message && *message ? message : "invalid JSON";
    jsc_context_clear_exception(context);
    return result;
}

bool parse_manifest(const gchar *manifest_path, ExtensionInfo &info, std::vector<ContentScriptSpec> &scripts) {
    gchar *contents = nullptr;
    gsize length = 0;
    GError *error = nullptr;
    if (!g_file_get_contents(manifest_path, &contents, &length, &error)) {
        info.error = error ? error->message : "Could not read manifest.json";
        g_clear_error(&error);
        return false;
    }

    JSCContext *context = jsc_context_new();
    JSCValue *root = jsc_value_new_from_json(context, contents);
    g_free(contents);

    if (!root || !jsc_value_is_object(root)) {
        info.error = "Invalid manifest.json: " + json_error(context);
        if (root)
            g_object_unref(root);
        g_object_unref(context);
        return false;
    }

    info.name = json_string(root, "name");
    info.version = json_string(root, "version");
    info.description = json_string(root, "description");

    JSCValue *content_scripts = jsc_value_object_get_property(root, "content_scripts");
    if (content_scripts && jsc_value_is_array(content_scripts)) {
        JSCValue *length_value = jsc_value_object_get_property(content_scripts, "length");
        int count = length_value ? jsc_value_to_int32(length_value) : 0;
        if (length_value)
            g_object_unref(length_value);

        for (int i = 0; i < count; i++) {
            JSCValue *entry = jsc_value_object_get_property_at_index(content_scripts, (guint)i);
            if (!entry || !jsc_value_is_object(entry)) {
                if (entry)
                    g_object_unref(entry);
                continue;
            }

            ContentScriptSpec spec;
            spec.matches = json_string_array(entry, "matches");
            spec.js = json_string_array(entry, "js");
            spec.css = json_string_array(entry, "css");
            spec.run_at = json_string(entry, "run_at");
            spec.all_frames = json_bool(entry, "all_frames");
            g_object_unref(entry);

            info.script_count += (int)spec.js.size();
            info.style_count += (int)spec.css.size();
            if (!spec.js.empty() || !spec.css.empty())
                scripts.push_back(spec);
        }
    }
    if (content_scripts)
        g_object_unref(content_scripts);

    g_object_unref(root);
    g_object_unref(context);

    if (info.name.empty())
        info.name = "Unnamed extension";
    return true;
}

std::string sanitize_identifier(const gchar *raw) {
    std::string result;
    for (const gchar *p = raw; p && *p; p++) {
        guchar c = (guchar)*p;
        if (g_ascii_isalnum(c) || c == '-' || c == '_' || c == '.') {
            result.push_back((char)g_ascii_tolower(c));
        } else if (!result.empty() && result.back() != '-') {
            result.push_back('-');
        }
    }
    while (!result.empty() && (result.back() == '-' || result.back() == '.'))
        result.pop_back();
    return result;
}

bool valid_identifier(const std::string &id) {
    if (id.empty() || id == "." || id == "..")
        return false;
    for (unsigned char c : id) {
        if (!(g_ascii_isalnum(c) || c == '-' || c == '_' || c == '.'))
            return false;
    }
    return true;
}

gchar *extension_path(const std::string &id) {
    return g_build_filename(extensions_directory(), id.c_str(), nullptr);
}

bool resolve_extension_file(const std::string &base, const std::string &relative, std::string &result) {
    if (relative.empty() || g_path_is_absolute(relative.c_str()))
        return false;

    gchar *base_canonical = g_canonicalize_filename(base.c_str(), nullptr);
    gchar *joined = g_build_filename(base.c_str(), relative.c_str(), nullptr);
    gchar *canonical = g_canonicalize_filename(joined, nullptr);

    std::string prefix = base_canonical;
    if (!prefix.empty() && prefix.back() != G_DIR_SEPARATOR)
        prefix.push_back(G_DIR_SEPARATOR);

    bool inside = g_str_has_prefix(canonical, prefix.c_str());
    if (inside)
        result = canonical;

    g_free(base_canonical);
    g_free(joined);
    g_free(canonical);
    return inside;
}

bool read_text_file(const std::string &path, std::string &contents) {
    gchar *text = nullptr;
    gsize length = 0;
    GError *error = nullptr;
    if (!g_file_get_contents(path.c_str(), &text, &length, &error)) {
        g_clear_error(&error);
        return false;
    }
    contents.assign(text, length);
    g_free(text);
    return true;
}

void append_allowed_patterns(const std::vector<std::string> &matches, std::vector<std::string> &allow,
                             bool &allow_file) {
    for (const auto &pattern : matches) {
        if (pattern == "<all_urls>") {
            allow.push_back("http://*/*");
            allow.push_back("https://*/*");
        } else if (pattern.rfind("*://", 0) == 0) {
            std::string rest = pattern.substr(4);
            allow.push_back("http://" + rest);
            allow.push_back("https://" + rest);
        } else if (pattern.rfind("http://", 0) == 0 || pattern.rfind("https://", 0) == 0) {
            allow.push_back(pattern);
        } else if (pattern.rfind("file://", 0) == 0) {
            allow.push_back(pattern);
            allow_file = true;
        }
    }
}

std::vector<const gchar *> cstr_list(const std::vector<std::string> &values) {
    std::vector<const gchar *> list;
    list.reserve(values.size() + 1);
    for (const auto &value : values)
        list.push_back(value.c_str());
    list.push_back(nullptr);
    return list;
}

void inject_content_scripts(ExtensionInfo &info, const std::vector<ContentScriptSpec> &scripts) {
    WebKitUserContentManager *manager = adblock_content_manager();
    if (!manager) {
        info.error = "The WebKit user content manager is unavailable.";
        return;
    }

    std::string world = "kite_ext_";
    for (unsigned char c : info.id)
        world.push_back((g_ascii_isalnum(c) || c == '_') ? (char)c : '_');

    for (const auto &spec : scripts) {
        std::vector<std::string> allow;
        bool allow_file = false;
        append_allowed_patterns(spec.matches, allow, allow_file);
        if (allow.empty()) {
            if (info.error.empty())
                info.error = "A content script has no supported match patterns.";
            continue;
        }

        std::vector<std::string> block = {"kite://*", "kite://*/*"};
        if (!allow_file)
            block.push_back("file:///*");

        auto allow_list = cstr_list(allow);
        auto block_list = cstr_list(block);
        WebKitUserContentInjectedFrames frames =
            spec.all_frames ? WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES : WEBKIT_USER_CONTENT_INJECT_TOP_FRAME;
        WebKitUserScriptInjectionTime injection_time =
            spec.run_at == "document_start" ? WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START
                                            : WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END;

        for (const auto &relative : spec.js) {
            std::string path;
            std::string source;
            if (!resolve_extension_file(info.path, relative, path) || !read_text_file(path, source)) {
                if (info.error.empty())
                    info.error = "Could not read script " + relative;
                continue;
            }
            WebKitUserScript *script = webkit_user_script_new_for_world(
                source.c_str(), frames, injection_time, world.c_str(), allow_list.data(), block_list.data());
            if (!script) {
                if (info.error.empty())
                    info.error = "WebKit rejected script " + relative;
                continue;
            }
            webkit_user_content_manager_add_script(manager, script);
            injected_scripts.push_back(script);
        }

        for (const auto &relative : spec.css) {
            std::string path;
            std::string source;
            if (!resolve_extension_file(info.path, relative, path) || !read_text_file(path, source)) {
                if (info.error.empty())
                    info.error = "Could not read style " + relative;
                continue;
            }
            WebKitUserStyleSheet *sheet = webkit_user_style_sheet_new(
                source.c_str(), frames, WEBKIT_USER_STYLE_LEVEL_USER, allow_list.data(), block_list.data());
            if (!sheet) {
                if (info.error.empty())
                    info.error = "WebKit rejected style " + relative;
                continue;
            }
            webkit_user_content_manager_add_style_sheet(manager, sheet);
            injected_styles.push_back(sheet);
        }
    }
}

void clear_injections() {
    WebKitUserContentManager *manager = adblock_content_manager();
    if (manager) {
        for (auto *script : injected_scripts)
            webkit_user_content_manager_remove_script(manager, script);
        for (auto *sheet : injected_styles)
            webkit_user_content_manager_remove_style_sheet(manager, sheet);
    }
    for (auto *script : injected_scripts)
        webkit_user_script_unref(script);
    for (auto *sheet : injected_styles)
        webkit_user_style_sheet_unref(sheet);
    injected_scripts.clear();
    injected_styles.clear();
}

bool copy_directory(const gchar *source, const gchar *destination, std::string &error) {
    if (g_mkdir_with_parents(destination, 0755) != 0) {
        error = g_strerror(errno);
        return false;
    }

    GError *dir_error = nullptr;
    GDir *dir = g_dir_open(source, 0, &dir_error);
    if (!dir) {
        error = dir_error ? dir_error->message : "Could not read the extension folder.";
        g_clear_error(&dir_error);
        return false;
    }

    bool ok = true;
    const gchar *name = nullptr;
    while (ok && (name = g_dir_read_name(dir))) {
        gchar *child_source = g_build_filename(source, name, nullptr);
        gchar *child_destination = g_build_filename(destination, name, nullptr);

        if (g_file_test(child_source, G_FILE_TEST_IS_DIR) &&
            !g_file_test(child_source, G_FILE_TEST_IS_SYMLINK)) {
            ok = copy_directory(child_source, child_destination, error);
        } else if (g_file_test(child_source, G_FILE_TEST_IS_REGULAR)) {
            GFile *from = g_file_new_for_path(child_source);
            GFile *to = g_file_new_for_path(child_destination);
            GError *copy_error = nullptr;
            if (!g_file_copy(from, to, G_FILE_COPY_OVERWRITE, nullptr, nullptr, nullptr, &copy_error)) {
                error = copy_error ? copy_error->message : "Could not copy an extension file.";
                g_clear_error(&copy_error);
                ok = false;
            }
            g_object_unref(from);
            g_object_unref(to);
        }

        g_free(child_source);
        g_free(child_destination);
    }
    g_dir_close(dir);
    return ok;
}

bool remove_directory(const gchar *path, std::string &error) {
    GDir *dir = g_dir_open(path, 0, nullptr);
    if (!dir) {
        error = g_strerror(errno);
        return false;
    }

    bool ok = true;
    const gchar *name = nullptr;
    while (ok && (name = g_dir_read_name(dir))) {
        gchar *child = g_build_filename(path, name, nullptr);
        if (g_file_test(child, G_FILE_TEST_IS_DIR) && !g_file_test(child, G_FILE_TEST_IS_SYMLINK)) {
            ok = remove_directory(child, error);
        } else if (g_remove(child) != 0) {
            error = g_strerror(errno);
            ok = false;
        }
        g_free(child);
    }
    g_dir_close(dir);

    if (ok && g_rmdir(path) != 0) {
        error = g_strerror(errno);
        ok = false;
    }
    return ok;
}

} // namespace

const gchar *extensions_directory() {
    if (!extension_root)
        extension_root = g_build_filename(g_get_user_data_dir(), "kite", "extensions", nullptr);
    return extension_root;
}

void extensions_reload() {
    clear_injections();
    loaded_extensions.clear();

    const gchar *root = extensions_directory();
    g_mkdir_with_parents(root, 0700);

    GDir *dir = g_dir_open(root, 0, nullptr);
    if (!dir)
        return;

    const gchar *name = nullptr;
    while ((name = g_dir_read_name(dir))) {
        if (name[0] == '.')
            continue;

        gchar *path = g_build_filename(root, name, nullptr);
        if (!g_file_test(path, G_FILE_TEST_IS_DIR)) {
            g_free(path);
            continue;
        }

        gchar *manifest_path = g_build_filename(path, "manifest.json", nullptr);
        if (!g_file_test(manifest_path, G_FILE_TEST_IS_REGULAR)) {
            g_free(manifest_path);
            g_free(path);
            continue;
        }

        ExtensionInfo info;
        info.id = name;
        info.path = path;

        std::vector<ContentScriptSpec> scripts;
        info.valid = parse_manifest(manifest_path, info, scripts);

        gchar *disabled_path = g_build_filename(path, "disabled", nullptr);
        bool disabled = g_file_test(disabled_path, G_FILE_TEST_EXISTS);
        g_free(disabled_path);
        info.enabled = info.valid && !disabled;

        if (info.name.empty())
            info.name = name;
        if (info.enabled)
            inject_content_scripts(info, scripts);

        loaded_extensions.push_back(info);
        g_free(manifest_path);
        g_free(path);
    }
    g_dir_close(dir);

    std::sort(loaded_extensions.begin(), loaded_extensions.end(), [](const ExtensionInfo &a, const ExtensionInfo &b) {
        return g_ascii_strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });

    g_print("[Kite] Loaded %zu extension(s).\n", loaded_extensions.size());
}

void extensions_init() {
    g_mkdir_with_parents(extensions_directory(), 0700);
    extensions_reload();
}

std::vector<ExtensionInfo> extensions_list() {
    return loaded_extensions;
}

bool extensions_install_from_folder(const gchar *source_dir, std::string *error) {
    std::string ignored;
    std::string &message = error ? *error : ignored;
    message.clear();

    if (!source_dir || !*source_dir) {
        message = "Choose an extension folder first.";
        return false;
    }

    gchar *source = g_canonicalize_filename(source_dir, nullptr);
    gchar *manifest_path = g_build_filename(source, "manifest.json", nullptr);
    if (!g_file_test(manifest_path, G_FILE_TEST_IS_REGULAR)) {
        message = "The selected folder does not contain a manifest.json file.";
        g_free(manifest_path);
        g_free(source);
        return false;
    }

    ExtensionInfo probe;
    std::vector<ContentScriptSpec> scripts;
    if (!parse_manifest(manifest_path, probe, scripts)) {
        message = probe.error.empty() ? "The extension manifest is invalid." : probe.error;
        g_free(manifest_path);
        g_free(source);
        return false;
    }

    gchar *basename = g_path_get_basename(source);
    std::string id = sanitize_identifier(basename);
    g_free(basename);
    if (id.empty())
        id = "extension";

    gchar *destination = g_build_filename(extensions_directory(), id.c_str(), nullptr);
    if (g_file_test(destination, G_FILE_TEST_EXISTS)) {
        message = "An extension folder named \"" + id + "\" is already installed. Remove it first to reinstall.";
        g_free(destination);
        g_free(manifest_path);
        g_free(source);
        return false;
    }

    bool copied = copy_directory(source, destination, message);
    g_free(manifest_path);
    g_free(source);
    if (!copied) {
        std::string cleanup_error;
        remove_directory(destination, cleanup_error);
        g_free(destination);
        return false;
    }

    gchar *disabled_path = g_build_filename(destination, "disabled", nullptr);
    g_remove(disabled_path);
    g_free(disabled_path);
    g_free(destination);

    extensions_reload();
    return true;
}

bool extensions_set_enabled(const gchar *id, bool enabled) {
    if (!id || !valid_identifier(id))
        return false;

    gchar *path = extension_path(id);
    if (!g_file_test(path, G_FILE_TEST_IS_DIR)) {
        g_free(path);
        return false;
    }

    gchar *disabled_path = g_build_filename(path, "disabled", nullptr);
    bool ok = enabled ? g_remove(disabled_path) == 0 || !g_file_test(disabled_path, G_FILE_TEST_EXISTS)
                      : g_file_set_contents(disabled_path, "Disabled from kite://extensions\n", -1, nullptr);
    g_free(disabled_path);
    g_free(path);

    if (ok)
        extensions_reload();
    return ok;
}

bool extensions_remove(const gchar *id) {
    if (!id || !valid_identifier(id))
        return false;

    gchar *path = extension_path(id);
    if (!g_file_test(path, G_FILE_TEST_IS_DIR)) {
        g_free(path);
        return false;
    }

    std::string error;
    bool ok = remove_directory(path, error);
    g_free(path);
    if (ok)
        extensions_reload();
    return ok;
}
