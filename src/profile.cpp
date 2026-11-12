#include "profile.h"

#include <glib/gstdio.h>

#include <algorithm>
#include <cerrno>

namespace {

constexpr const char *kDefaultProfileId = "default";
constexpr const char *kDefaultProfileName = "Default";
constexpr const char *kGuestProfileId = "guest";
constexpr const char *kGuestProfileName = "Guest";

std::string current_id = kDefaultProfileId;
std::string current_name = kDefaultProfileName;
std::string guest_root;
bool guest_active = false;

struct AvatarColor {
    double red;
    double green;
    double blue;
};

// Keep this palette in sync with the avatar-color-* CSS classes.
constexpr AvatarColor kAvatarColors[] = {
    {0.10, 0.45, 0.91}, // blue
    {0.85, 0.27, 0.22}, // red
    {0.18, 0.65, 0.38}, // green
    {0.85, 0.55, 0.10}, // amber
    {0.55, 0.35, 0.86}, // purple
    {0.05, 0.62, 0.66}, // teal
    {0.90, 0.35, 0.55}, // pink
    {0.45, 0.50, 0.56}, // slate
};
constexpr int kAvatarColorCount = (int)(sizeof(kAvatarColors) / sizeof(kAvatarColors[0]));

gchar *cached_data_dir = nullptr;
gchar *cached_config_dir = nullptr;
gchar *cached_cache_dir = nullptr;
gchar *cached_storage_dir = nullptr;
gchar *cached_webkit_data_dir = nullptr;
gchar *cached_webkit_cache_dir = nullptr;

std::string base_data_path() {
    gchar *path = g_build_filename(g_get_user_data_dir(), "kite", nullptr);
    std::string result = path;
    g_free(path);
    return result;
}

std::string base_config_path() {
    gchar *path = g_build_filename(g_get_user_config_dir(), "kite", nullptr);
    std::string result = path;
    g_free(path);
    return result;
}

std::string base_cache_path() {
    gchar *path = g_build_filename(g_get_user_cache_dir(), "kite", nullptr);
    std::string result = path;
    g_free(path);
    return result;
}

gchar *registry_path() {
    std::string base = base_config_path();
    return g_build_filename(base.c_str(), "profiles.ini", nullptr);
}

gchar *active_marker_path() {
    std::string base = base_config_path();
    return g_build_filename(base.c_str(), "active-profile", nullptr);
}

std::string profile_subdir(const std::string &root, const std::string &id) {
    if (id.empty() || id == kDefaultProfileId)
        return root;
    gchar *path = g_build_filename(root.c_str(), "profiles", id.c_str(), nullptr);
    std::string result = path;
    g_free(path);
    return result;
}

void clear_cached_paths() {
    g_free(cached_data_dir);
    g_free(cached_config_dir);
    g_free(cached_cache_dir);
    g_free(cached_storage_dir);
    g_free(cached_webkit_data_dir);
    g_free(cached_webkit_cache_dir);
    cached_data_dir = nullptr;
    cached_config_dir = nullptr;
    cached_cache_dir = nullptr;
    cached_storage_dir = nullptr;
    cached_webkit_data_dir = nullptr;
    cached_webkit_cache_dir = nullptr;
}

void rebuild_cached_paths() {
    clear_cached_paths();
    std::string data;
    std::string config;
    std::string cache;
    if (current_id == kGuestProfileId && !guest_root.empty()) {
        data = guest_root;
        config = std::string(guest_root) + "/config";
        cache = std::string(guest_root) + "/cache";
    } else {
        data = profile_subdir(base_data_path(), current_id);
        config = profile_subdir(base_config_path(), current_id);
        cache = profile_subdir(base_cache_path(), current_id);
    }

    cached_data_dir = g_strdup(data.c_str());
    cached_config_dir = g_strdup(config.c_str());
    cached_cache_dir = g_strdup(cache.c_str());

    gchar *storage = g_build_filename(data.c_str(), "extension-storage", nullptr);
    cached_storage_dir = storage;

    gchar *webkit_data = g_build_filename(data.c_str(), "webkit", nullptr);
    cached_webkit_data_dir = webkit_data;
    gchar *webkit_cache = g_build_filename(cache.c_str(), "webkit", nullptr);
    cached_webkit_cache_dir = webkit_cache;
}

std::string trimmed_name(const gchar *name) {
    if (!name)
        return "";
    gchar *copy = g_strdup(name);
    gchar *stripped = g_strstrip(copy);
    std::string result = stripped ? stripped : "";
    g_free(copy);
    return result;
}

bool valid_profile_id(const std::string &id) {
    if (id.empty() || id == "." || id == "..")
        return false;
    for (unsigned char c : id) {
        if (!(g_ascii_isalnum(c) || c == '-' || c == '_'))
            return false;
    }
    return true;
}

std::string make_profile_id(const std::string &name) {
    std::string id;
    for (unsigned char c : name) {
        if (g_ascii_isalnum(c))
            id.push_back((char)g_ascii_tolower(c));
        else if ((c == ' ' || c == '-' || c == '_' || c == '.') && !id.empty() && id.back() != '-')
            id.push_back('-');
    }
    while (!id.empty() && id.back() == '-')
        id.pop_back();
    if (id.empty())
        id = "profile";
    if (id == kDefaultProfileId)
        id = "profile";
    return id;
}

struct Registry {
    std::vector<ProfileInfo> profiles;
    int next_id = 1;

    bool exists(const std::string &id) const {
        for (const auto &profile : profiles) {
            if (profile.id == id)
                return true;
        }
        return false;
    }

    ProfileInfo *find(const std::string &id) {
        for (auto &profile : profiles) {
            if (profile.id == id)
                return &profile;
        }
        return nullptr;
    }
};

Registry load_registry() {
    Registry registry;
    gchar *path = registry_path();
    GKeyFile *file = g_key_file_new();
    GError *error = nullptr;
    if (!g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, &error))
        g_clear_error(&error);

    gint next = g_key_file_get_integer(file, "meta", "next_id", nullptr);
    if (next > 0)
        registry.next_id = next;

    gsize count = 0;
    gchar **keys = g_key_file_get_keys(file, "profiles", &count, nullptr);
    if (keys) {
        for (gsize i = 0; i < count; i++) {
            std::string id = keys[i];
            if (!valid_profile_id(id))
                continue;
            gchar *name = g_key_file_get_string(file, "profiles", keys[i], nullptr);
            ProfileInfo info;
            info.id = id;
            info.name = trimmed_name(name);
            if (info.name.empty())
                info.name = id;
            info.is_default = id == kDefaultProfileId;
            registry.profiles.push_back(info);
            g_free(name);
        }
        g_strfreev(keys);
    }
    g_key_file_free(file);
    g_free(path);

    if (!registry.exists(kDefaultProfileId)) {
        ProfileInfo info;
        info.id = kDefaultProfileId;
        info.name = kDefaultProfileName;
        info.is_default = true;
        registry.profiles.insert(registry.profiles.begin(), info);
    }

    std::sort(registry.profiles.begin(), registry.profiles.end(), [](const ProfileInfo &a, const ProfileInfo &b) {
        if (a.is_default != b.is_default)
            return a.is_default;
        return g_ascii_strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return registry;
}

bool save_registry(const Registry &registry, std::string *error) {
    GKeyFile *file = g_key_file_new();
    g_key_file_set_integer(file, "meta", "next_id", registry.next_id);
    for (const auto &profile : registry.profiles)
        g_key_file_set_string(file, "profiles", profile.id.c_str(), profile.name.c_str());

    gchar *path = registry_path();
    gchar *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);

    gsize length = 0;
    gchar *data = g_key_file_to_data(file, &length, nullptr);
    GError *write_error = nullptr;
    bool ok = g_file_set_contents(path, data, (gssize)length, &write_error);
    if (!ok && error)
        *error = write_error ? write_error->message : "Could not save the profile registry.";
    g_clear_error(&write_error);
    if (ok)
        g_chmod(path, 0600);
    g_free(data);
    g_free(path);
    g_key_file_free(file);
    return ok;
}

bool remove_tree(const gchar *path, std::string *error) {
    if (!g_file_test(path, G_FILE_TEST_EXISTS))
        return true;

    GDir *dir = g_dir_open(path, 0, nullptr);
    if (!dir) {
        if (error)
            *error = g_strerror(errno);
        return false;
    }

    bool ok = true;
    const gchar *name = nullptr;
    while (ok && (name = g_dir_read_name(dir))) {
        gchar *child = g_build_filename(path, name, nullptr);
        if (g_file_test(child, G_FILE_TEST_IS_DIR) && !g_file_test(child, G_FILE_TEST_IS_SYMLINK))
            ok = remove_tree(child, error);
        else if (g_remove(child) != 0) {
            if (error)
                *error = g_strerror(errno);
            ok = false;
        }
        g_free(child);
    }
    g_dir_close(dir);
    if (ok && g_rmdir(path) != 0) {
        if (error)
            *error = g_strerror(errno);
        ok = false;
    }
    return ok;
}

void ensure_directory(const gchar *path) {
    if (path)
        g_mkdir_with_parents(path, 0700);
}

void write_active_marker(const std::string &id) {
    std::string base = base_config_path();
    g_mkdir_with_parents(base.c_str(), 0700);
    gchar *path = active_marker_path();
    gchar *contents = g_strdup_printf("%s\n", id.c_str());
    g_file_set_contents(path, contents, -1, nullptr);
    g_chmod(path, 0600);
    g_free(contents);
    g_free(path);
}

std::string read_active_marker() {
    gchar *path = active_marker_path();
    gchar *contents = nullptr;
    if (!g_file_get_contents(path, &contents, nullptr, nullptr)) {
        g_free(path);
        return "";
    }
    g_free(path);
    gchar *stripped = g_strstrip(contents);
    std::string result = stripped ? stripped : "";
    g_free(contents);
    return result;
}

} // namespace

void profile_init(const gchar *requested_id) {
    if (current_id == kGuestProfileId)
        profile_cleanup_guest();

    std::string wanted = requested_id ? requested_id : "";
    if (wanted.empty())
        wanted = read_active_marker();

    if (wanted == kGuestProfileId) {
        profile_init_guest();
        return;
    }

    Registry registry = load_registry();
    if (!wanted.empty() && valid_profile_id(wanted) && registry.exists(wanted)) {
        current_id = wanted;
    } else {
        if (!wanted.empty())
            g_printerr("[Kite] Profile \"%s\" does not exist; using the default profile.\n", wanted.c_str());
        current_id = kDefaultProfileId;
    }

    ProfileInfo *selected = registry.find(current_id);
    current_name = selected && !selected->name.empty() ? selected->name : current_id;
    rebuild_cached_paths();

    ensure_directory(cached_data_dir);
    ensure_directory(cached_config_dir);
    ensure_directory(cached_cache_dir);
    ensure_directory(cached_storage_dir);
    if (!profile_is_default()) {
        ensure_directory(cached_webkit_data_dir);
        ensure_directory(cached_webkit_cache_dir);
    }
    write_active_marker(current_id);

    gchar *path = registry_path();
    bool registry_missing = !g_file_test(path, G_FILE_TEST_EXISTS);
    g_free(path);
    if (registry_missing)
        save_registry(registry, nullptr);
}

void profile_init_guest() {
    if (!guest_root.empty()) {
        std::string previous = guest_root;
        guest_root.clear();
        remove_tree(previous.c_str(), nullptr);
    }

    GError *error = nullptr;
    gchar *root = g_dir_make_tmp("kite-guest-XXXXXX", &error);
    if (!root) {
        g_clear_error(&error);
        gchar *fallback =
            g_strdup_printf("%s/kite-guest-%08x", g_get_user_runtime_dir(), g_random_int());
        root = fallback;
    }

    guest_root = root;
    g_free(root);
    guest_active = true;
    current_id = kGuestProfileId;
    current_name = kGuestProfileName;
    rebuild_cached_paths();

    ensure_directory(cached_data_dir);
    ensure_directory(cached_config_dir);
    ensure_directory(cached_cache_dir);
    ensure_directory(cached_storage_dir);
    ensure_directory(cached_webkit_data_dir);
    ensure_directory(cached_webkit_cache_dir);

    g_printerr("[Kite] Using a temporary guest profile at %s\n", guest_root.c_str());
}

bool profile_is_guest() {
    return guest_active || current_id == kGuestProfileId;
}

void profile_cleanup_guest() {
    if (!guest_root.empty()) {
        remove_tree(guest_root.c_str(), nullptr);
        guest_root.clear();
    }
    guest_active = false;
    clear_cached_paths();
    if (current_id == kGuestProfileId) {
        current_id = kDefaultProfileId;
        current_name = kDefaultProfileName;
    }
}

const gchar *profile_current_id() {
    return current_id.c_str();
}

const gchar *profile_current_name() {
    return current_name.c_str();
}

bool profile_is_default() {
    return current_id == kDefaultProfileId;
}

std::string profile_avatar_text(const std::string &name) {
    std::string trimmed = trimmed_name(name.c_str());
    if (trimmed.empty())
        return "?";

    gchar **words = g_strsplit_set(trimmed.c_str(), " \t-_", -1);
    std::string result;
    for (gchar **word = words; *word && result.size() < 2; word++) {
        if (!**word)
            continue;
        const gchar *p = *word;
        gunichar c = g_utf8_get_char(p);
        if (!c)
            continue;
        gchar buffer[8] = {0};
        int length = g_unichar_to_utf8(g_unichar_toupper(c), buffer);
        result.append(buffer, length);
    }
    g_strfreev(words);
    return result.empty() ? "?" : result;
}

int profile_avatar_color(const std::string &id) {
    if (id == kGuestProfileId)
        return -1;
    return (int)(g_str_hash(id.c_str()) % kAvatarColorCount);
}

void profile_avatar_rgb(const std::string &id, double *red, double *green, double *blue) {
    int index = profile_avatar_color(id);
    AvatarColor color = index < 0 ? AvatarColor{0.45, 0.50, 0.56} : kAvatarColors[index];
    if (red)
        *red = color.red;
    if (green)
        *green = color.green;
    if (blue)
        *blue = color.blue;
}

std::vector<ProfileInfo> profile_list() {
    Registry registry = load_registry();
    std::vector<ProfileInfo> profiles = registry.profiles;
    for (auto &profile : profiles)
        profile.current = profile.id == current_id;
    return profiles;
}

bool profile_exists(const gchar *id) {
    if (!id || !valid_profile_id(id))
        return false;
    if (std::string(id) == kGuestProfileId)
        return true;
    Registry registry = load_registry();
    return registry.exists(id);
}

bool profile_create(const gchar *name, std::string *error, std::string *new_id) {
    std::string display_name = trimmed_name(name);
    if (display_name.empty()) {
        if (error)
            *error = "Enter a profile name.";
        return false;
    }

    Registry registry = load_registry();
    std::string id = make_profile_id(display_name);
    std::string base_id = id;
    int suffix = 2;
    while (registry.exists(id)) {
        id = base_id + "-" + std::to_string(suffix++);
    }

    ProfileInfo info;
    info.id = id;
    info.name = display_name;
    info.is_default = false;
    registry.profiles.push_back(info);

    if (!save_registry(registry, error))
        return false;

    std::string data = profile_subdir(base_data_path(), id);
    std::string config = profile_subdir(base_config_path(), id);
    std::string cache = profile_subdir(base_cache_path(), id);
    g_mkdir_with_parents(data.c_str(), 0700);
    g_mkdir_with_parents(config.c_str(), 0700);
    g_mkdir_with_parents(cache.c_str(), 0700);
    gchar *webkit_data = g_build_filename(data.c_str(), "webkit", nullptr);
    gchar *webkit_cache = g_build_filename(cache.c_str(), "webkit", nullptr);
    gchar *storage = g_build_filename(data.c_str(), "extension-storage", nullptr);
    g_mkdir_with_parents(webkit_data, 0700);
    g_mkdir_with_parents(webkit_cache, 0700);
    g_mkdir_with_parents(storage, 0700);
    g_free(webkit_data);
    g_free(webkit_cache);
    g_free(storage);

    if (new_id)
        *new_id = id;
    return true;
}

bool profile_rename(const gchar *id, const gchar *name, std::string *error) {
    if (!id || !valid_profile_id(id)) {
        if (error)
            *error = "Unknown profile.";
        return false;
    }
    std::string display_name = trimmed_name(name);
    if (display_name.empty()) {
        if (error)
            *error = "Enter a profile name.";
        return false;
    }

    Registry registry = load_registry();
    ProfileInfo *profile = registry.find(id);
    if (!profile) {
        if (error)
            *error = "Unknown profile.";
        return false;
    }
    profile->name = display_name;
    if (std::string(id) == current_id)
        current_name = display_name;
    return save_registry(registry, error);
}

bool profile_delete(const gchar *id, std::string *error) {
    if (!id || !valid_profile_id(id)) {
        if (error)
            *error = "Unknown profile.";
        return false;
    }
    if (std::string(id) == kDefaultProfileId) {
        if (error)
            *error = "The default profile cannot be deleted.";
        return false;
    }
    if (std::string(id) == current_id) {
        if (error)
            *error = "Switch to another profile before deleting the current one.";
        return false;
    }

    Registry registry = load_registry();
    if (!registry.find(id)) {
        if (error)
            *error = "Unknown profile.";
        return false;
    }

    std::string data = profile_subdir(base_data_path(), id);
    std::string config = profile_subdir(base_config_path(), id);
    std::string cache = profile_subdir(base_cache_path(), id);
    std::string remove_error;
    if (!remove_tree(data.c_str(), &remove_error) || !remove_tree(config.c_str(), &remove_error) ||
        !remove_tree(cache.c_str(), &remove_error)) {
        if (error)
            *error = remove_error.empty() ? "Could not remove the profile directory." : remove_error;
        return false;
    }

    auto end = std::remove_if(registry.profiles.begin(), registry.profiles.end(),
                              [id](const ProfileInfo &profile) { return profile.id == id; });
    registry.profiles.erase(end, registry.profiles.end());
    return save_registry(registry, error);
}

bool profile_switch(const gchar *id) {
    if (!id || !valid_profile_id(id))
        return false;

    bool guest = std::string(id) == kGuestProfileId;
    if (!guest) {
        Registry registry = load_registry();
        if (!registry.exists(id))
            return false;
        write_active_marker(id);
    }

    gchar *exe = g_file_read_link("/proc/self/exe", nullptr);
    if (!exe)
        exe = g_strdup(g_get_prgname());
    if (!exe)
        return false;

    gchar *argument = guest ? g_strdup("--guest") : g_strdup_printf("--profile=%s", id);
    gchar *argv[] = {exe, argument, nullptr};
    GError *error = nullptr;
    gboolean spawned = g_spawn_async(nullptr, argv, nullptr, G_SPAWN_DEFAULT, nullptr, nullptr, nullptr, &error);
    if (!spawned) {
        g_printerr("[Kite] Could not launch the %s profile: %s\n", id,
                   error ? error->message : "unknown error");
        g_clear_error(&error);
    }
    g_free(argument);
    g_free(exe);
    return spawned;
}

bool profile_switch_guest() {
    return profile_switch(kGuestProfileId);
}

const gchar *profile_data_dir() {
    if (!cached_data_dir)
        rebuild_cached_paths();
    return cached_data_dir;
}

const gchar *profile_config_dir() {
    if (!cached_config_dir)
        rebuild_cached_paths();
    return cached_config_dir;
}

const gchar *profile_cache_dir() {
    if (!cached_cache_dir)
        rebuild_cached_paths();
    return cached_cache_dir;
}

const gchar *profile_extension_storage_dir() {
    if (!cached_storage_dir)
        rebuild_cached_paths();
    ensure_directory(cached_storage_dir);
    return cached_storage_dir;
}

const gchar *profile_webkit_data_dir() {
    if (!cached_webkit_data_dir)
        rebuild_cached_paths();
    ensure_directory(cached_webkit_data_dir);
    return cached_webkit_data_dir;
}

const gchar *profile_webkit_cache_dir() {
    if (!cached_webkit_cache_dir)
        rebuild_cached_paths();
    ensure_directory(cached_webkit_cache_dir);
    return cached_webkit_cache_dir;
}

const gchar *profile_base_data_dir() {
    static gchar *base = nullptr;
    if (!base)
        base = g_strdup(base_data_path().c_str());
    return base;
}

const gchar *profile_base_config_dir() {
    static gchar *base = nullptr;
    if (!base)
        base = g_strdup(base_config_path().c_str());
    return base;
}
