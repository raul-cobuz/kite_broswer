#include "profile.h"

#include <glib.h>
#include <glib/gstdio.h>

#include <cstdio>
#include <string>

static int failures = 0;

static void check(bool condition, const char *label) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", label);
        failures++;
    }
}

int main() {
    GError *error = nullptr;
    gchar *tmp = g_dir_make_tmp("kite-profile-XXXXXX", &error);
    if (!tmp) {
        std::fprintf(stderr, "Could not create temp dir: %s\n", error ? error->message : "unknown error");
        if (error)
            g_error_free(error);
        return 1;
    }

    gchar *config_home = g_build_filename(tmp, "config", nullptr);
    gchar *data_home = g_build_filename(tmp, "data", nullptr);
    gchar *cache_home = g_build_filename(tmp, "cache", nullptr);
    g_mkdir_with_parents(config_home, 0700);
    g_mkdir_with_parents(data_home, 0700);
    g_mkdir_with_parents(cache_home, 0700);
    g_setenv("XDG_CONFIG_HOME", config_home, TRUE);
    g_setenv("XDG_DATA_HOME", data_home, TRUE);
    g_setenv("XDG_CACHE_HOME", cache_home, TRUE);
    g_setenv("TMPDIR", tmp, TRUE);

    profile_init(nullptr);
    check(std::string(profile_current_id()) == "default", "default profile selected");
    check(profile_is_default(), "default profile flag");
    check(std::string(profile_config_dir()) == std::string(config_home) + "/kite", "default config dir");
    check(std::string(profile_data_dir()) == std::string(data_home) + "/kite", "default data dir");

    std::string create_error;
    std::string work_id;
    check(profile_create("Work", &create_error, &work_id), "profile created");
    check(work_id == "work", "profile id sanitized");
    check(profile_exists("work"), "profile exists after creation");

    std::vector<ProfileInfo> profiles = profile_list();
    check(profiles.size() == 2, "two profiles listed");
    if (profiles.size() == 2) {
        check(profiles[0].is_default, "default profile sorted first");
        check(profiles[0].current, "default profile is current");
        check(profiles[1].id == "work", "named profile listed");
    }

    profile_init("work");
    check(std::string(profile_current_id()) == "work", "named profile selected");
    check(!profile_is_default(), "named profile is not default");
    std::string expected_config = std::string(config_home) + "/kite/profiles/work";
    std::string expected_data = std::string(data_home) + "/kite/profiles/work";
    check(std::string(profile_config_dir()) == expected_config, "named config dir isolated");
    check(std::string(profile_data_dir()) == expected_data, "named data dir isolated");
    check(std::string(profile_webkit_data_dir()).rfind(expected_data, 0) == 0, "named WebKit data dir isolated");

    std::string rename_error;
    check(profile_rename("work", "Office", &rename_error), "profile renamed");
    profiles = profile_list();
    bool found_office = false;
    for (const auto &profile : profiles) {
        if (profile.id == "work" && profile.name == "Office")
            found_office = true;
    }
    check(found_office, "renamed profile keeps its id");
    check(std::string(profile_current_name()) == "Office", "current profile name updated");

    std::string delete_error;
    check(!profile_delete("default", &delete_error), "default profile cannot be deleted");
    check(!profile_delete("work", &delete_error), "current profile cannot be deleted");

    profile_init("default");
    check(profile_delete("work", &delete_error), "non-current profile deleted");
    check(!profile_exists("work"), "deleted profile is gone");

    profile_init(nullptr);
    check(std::string(profile_current_id()) == "default", "active marker returns to default");

    check(profile_avatar_text("Work") == "W", "single-word avatar initial");
    check(profile_avatar_text("Ada Lovelace") == "AL", "two-word avatar initials");
    check(profile_avatar_text("  ") == "?", "empty avatar name fallback");
    int default_color = profile_avatar_color("default");
    check(default_color >= 0 && default_color < 8, "avatar color is in range");
    check(profile_avatar_color("default") == default_color, "avatar color is deterministic");
    check(profile_avatar_color("guest") == -1, "guest uses the neutral avatar color");
    double red = 0, green = 0, blue = 0;
    profile_avatar_rgb("guest", &red, &green, &blue);
    check(red == 0.45 && green == 0.50 && blue == 0.56, "guest avatar RGB fallback");
    profile_avatar_rgb("default", &red, &green, &blue);
    check(red >= 0 && red <= 1 && green >= 0 && green <= 1 && blue >= 0 && blue <= 1,
          "named avatar RGB is normalized");

    profile_init_guest();
    check(profile_is_guest(), "guest profile becomes current");
    check(std::string(profile_current_id()) == "guest", "guest profile id");
    check(std::string(profile_current_name()) == "Guest", "guest profile display name");
    check(std::string(profile_data_dir()) != expected_data, "guest data does not use the named profile directory");
    std::string guest_data = profile_data_dir();
    check(g_file_test(guest_data.c_str(), G_FILE_TEST_IS_DIR), "guest data directory created");
    size_t registry_count = profile_list().size();
    check(registry_count <= 2, "guest profiles stay out of the registry");
    bool guest_listed = false;
    for (const auto &profile : profile_list()) {
        if (profile.id == "guest")
            guest_listed = true;
    }
    check(!guest_listed, "guest profile is not listed with named profiles");
    profile_cleanup_guest();
    check(!profile_is_guest(), "guest profile cleared");
    check(!g_file_test(guest_data.c_str(), G_FILE_TEST_EXISTS), "guest data removed on cleanup");
    check(std::string(profile_current_id()) == "default", "cleanup returns to the default profile");

    g_free(cache_home);
    g_free(data_home);
    g_free(config_home);
    g_free(tmp);

    if (failures) {
        std::fprintf(stderr, "%d profile test(s) failed\n", failures);
        return 1;
    }
    std::printf("profile tests passed\n");
    return 0;
}
