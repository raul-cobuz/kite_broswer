#include "session.h"

#include <glib.h>
#include <glib/gstdio.h>

#include <cstdio>

static int failures = 0;

static void check(bool condition, const char *label) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", label);
        failures++;
    }
}

int main() {
    GError *error = nullptr;
    gchar *tmp = g_dir_make_tmp("kite-session-XXXXXX", &error);
    if (!tmp) {
        std::fprintf(stderr, "Could not create temp dir: %s\n", error ? error->message : "unknown error");
        if (error)
            g_error_free(error);
        return 1;
    }
    g_setenv("XDG_CONFIG_HOME", tmp, TRUE);
    session_clear();

    SessionState state;
    check(!session_load(&state), "missing session is not loaded");

    SessionState saved;
    saved.urls = {"https://example.com/", "kite://newtab", "https://other.test/page"};
    saved.active_index = 2;
    session_save(saved);

    gchar *session_file = g_build_filename(tmp, "kite", "session.ini", NULL);
    check(g_file_test(session_file, G_FILE_TEST_IS_REGULAR), "session file written");

    SessionState loaded;
    check(session_load(&loaded), "session loads");
    check(loaded.urls.size() == 3, "tab count restored");
    if (loaded.urls.size() == 3) {
        check(loaded.urls[0] == "https://example.com/", "first tab restored");
        check(loaded.urls[1] == "kite://newtab", "new tab page restored");
        check(loaded.urls[2] == "https://other.test/page", "third tab restored");
    }
    check(loaded.active_index == 2, "active tab restored");

    saved.active_index = 99;
    session_save(saved);
    check(session_load(&loaded), "session reloads");
    check(loaded.active_index == 2, "active tab is clamped");

    saved.urls.clear();
    session_save(saved);
    check(!g_file_test(session_file, G_FILE_TEST_EXISTS), "empty session removes file");
    check(!session_load(&loaded), "empty session is not loaded");

    session_clear();
    g_free(session_file);
    gchar *kite_dir = g_build_filename(tmp, "kite", NULL);
    g_rmdir(kite_dir);
    g_free(kite_dir);
    g_rmdir(tmp);
    g_free(tmp);

    if (failures) {
        std::fprintf(stderr, "%d session test(s) failed\n", failures);
        return 1;
    }
    std::printf("session tests passed\n");
    return 0;
}
