#include <glib-unix.h>
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

#include <cstring>

#include "adblock.h"
#include "app_context.h"
#include "browser_window.h"
#include "dark_mode.h"
#include "downloads.h"
#include "extensions.h"
#include "history.h"
#include "kite_pages.h"
#include "privacy.h"
#include "profile.h"
#include "settings.h"
#include "sync.h"
#include "tracking_protection.h"

static gboolean on_quit_signal(gpointer user_data) {
    (void)user_data;
    save_browser_session();
    save_settings();
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}

int main(int argc, char *argv[]) {
    gchar *requested_profile = nullptr;
    gchar *startup_url = nullptr;
    bool guest = false;
    int kept = 1;
    for (int i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "--guest") == 0) {
            guest = true;
        } else if (g_str_has_prefix(argv[i], "--profile=")) {
            g_free(requested_profile);
            requested_profile = g_strdup(argv[i] + strlen("--profile="));
        } else if (g_strcmp0(argv[i], "--profile") == 0 && i + 1 < argc) {
            g_free(requested_profile);
            requested_profile = g_strdup(argv[++i]);
        } else if (g_str_has_prefix(argv[i], "--url=")) {
            g_free(startup_url);
            startup_url = g_strdup(argv[i] + strlen("--url="));
        } else if (g_strcmp0(argv[i], "--url") == 0 && i + 1 < argc) {
            g_free(startup_url);
            startup_url = g_strdup(argv[++i]);
        } else if (argv[i][0] != '-' && !startup_url) {
            startup_url = g_strdup(argv[i]);
        } else {
            argv[kept++] = argv[i];
        }
    }
    argv[kept] = nullptr;
    argc = kept;

    gtk_init(&argc, &argv);
    g_set_prgname("kite");
    g_set_application_name("Kite Browser");
    setup_kite_window_icon();

    if (guest)
        profile_init_guest();
    else
        profile_init(requested_profile);
    g_free(requested_profile);

    load_settings();
    history_init();

    if (settings_sync_on_startup() && *settings_sync_folder()) {
        SyncResult sync_result;
        std::string sync_error;
        if (sync_run(settings_sync_folder(), settings_sync_settings(), &sync_result, &sync_error)) {
            g_print("[Kite] Synced with %s: %d bookmark(s), %d pin(s), %d history entr%s.\n",
                    settings_sync_folder(), sync_result.bookmarks_added, sync_result.pins_added,
                    sync_result.history_added, sync_result.history_added == 1 ? "y" : "ies");
        } else {
            g_printerr("[Kite] Sync failed: %s\n", sync_error.c_str());
        }
    }

    save_settings();

    static const char *BROWSER_CSS = R"CSS(
        window.kite-window { background-color: #f1f3f4; }
        .kite-toolbar {
            background-color: #f8f9fa;
            border-bottom: 1px solid rgba(0, 0, 0, 0.08);
            padding: 7px 10px;
        }
        .kite-toolbar button {
            background: transparent;
            background-image: none;
            border: none;
            box-shadow: none;
            border-radius: 999px;
            padding: 6px;
            min-width: 16px;
            min-height: 16px;
            color: #3c4043;
        }
        .kite-toolbar button:hover { background-color: rgba(60, 64, 67, 0.08); }
        .kite-toolbar button:active,
        .kite-toolbar button:checked { background-color: rgba(26, 115, 232, 0.12); color: #1a73e8; }
        .kite-toolbar entry.kite-omnibox {
            background-color: #ffffff;
            background-image: none;
            color: #202124;
            border: 1px solid #dadce0;
            border-radius: 999px;
            padding: 6px 15px;
            min-height: 26px;
            box-shadow: none;
        }
        .kite-toolbar entry.kite-omnibox:focus {
            border-color: #1a73e8;
            box-shadow: 0 0 0 2px rgba(26, 115, 232, 0.18);
        }
        .kite-toolbar entry.kite-omnibox image { color: #5f6368; }
        .kite-toolbar entry.incognito-mode {
            background-color: #241533;
            color: #d7c2ff;
            border-color: #8e44ad;
        }
        notebook.kite-notebook > header {
            background-color: #f8f9fa;
            border-bottom: 1px solid rgba(0, 0, 0, 0.06);
        }
        notebook.kite-notebook > header > tabs > tab {
            background-color: transparent;
            background-image: none;
            border: none;
            box-shadow: none;
            border-radius: 8px 8px 0 0;
            padding: 5px 9px;
            margin: 3px 2px 0 2px;
            min-height: 27px;
        }
        notebook.kite-notebook > header > tabs > tab:hover { background-color: rgba(60, 64, 67, 0.07); }
        notebook.kite-notebook > header > tabs > tab:checked {
            background-color: #ffffff;
            box-shadow: 0 1px 2px rgba(0, 0, 0, 0.08);
        }
        notebook.kite-notebook > header > tabs > tab button {
            background: transparent;
            border: none;
            box-shadow: none;
            border-radius: 999px;
            padding: 1px;
            min-width: 15px;
            min-height: 15px;
        }
        notebook.kite-notebook > header > tabs > tab button:hover { background-color: rgba(60, 64, 67, 0.12); }
        notebook.kite-notebook > header > tabs > tab label { color: #3c4043; }
        notebook.kite-split-notebook { border-left: 1px solid rgba(0, 0, 0, 0.08); }
        paned.kite-split-pane > separator { background-color: rgba(0, 0, 0, 0.10); min-width: 1px; }
        window.kite-dark notebook.kite-notebook > header > tabs > tab label { color: #e8eaed; }
        window.kite-dark notebook.kite-notebook > header > tabs > tab button { color: #9aa0a6; }
        progressbar.kite-progress { min-height: 3px; }
        progressbar.kite-progress trough { min-height: 3px; background: transparent; border: none; box-shadow: none; }
        progressbar.kite-progress progress { min-height: 3px; background-color: #1a73e8; border-radius: 0; }

        window.kite-dark.kite-window { background-color: #121214; }
        window.kite-dark .kite-toolbar {
            background-color: #1b1b1f;
            border-bottom-color: #303036;
        }
        window.kite-dark .kite-toolbar button { color: #e8eaed; }
        window.kite-dark .kite-toolbar button:hover { background-color: rgba(232, 234, 237, 0.10); }
        window.kite-dark .kite-toolbar button:active,
        window.kite-dark .kite-toolbar button:checked {
            background-color: rgba(138, 180, 248, 0.18);
            color: #8ab4f8;
        }
        window.kite-dark .kite-toolbar entry.kite-omnibox {
            background-color: #202125;
            color: #e8eaed;
            border-color: #3c4043;
        }
        window.kite-dark .kite-toolbar entry.kite-omnibox:focus {
            border-color: #8ab4f8;
            box-shadow: 0 0 0 2px rgba(138, 180, 248, 0.20);
        }
        window.kite-dark .kite-toolbar entry.kite-omnibox image { color: #9aa0a6; }
        window.kite-dark notebook.kite-notebook > header {
            background-color: #1b1b1f;
            border-bottom-color: #303036;
        }
        window.kite-dark notebook.kite-notebook > header > tabs > tab:hover {
            background-color: rgba(232, 234, 237, 0.08);
        }
        window.kite-dark notebook.kite-notebook > header > tabs > tab:checked {
            background-color: #202125;
            box-shadow: 0 -1px 2px rgba(0, 0, 0, 0.35);
        }
        window.kite-dark notebook.kite-split-notebook { border-left-color: #303036; }
        window.kite-dark paned.kite-split-pane > separator { background-color: #303036; }

        /* Profile avatar in the toolbar */
        .kite-toolbar label.profile-avatar {
            min-width: 22px;
            min-height: 22px;
            padding: 1px;
            border-radius: 999px;
            color: #ffffff;
            font-size: 10px;
            font-weight: 700;
        }
        .kite-toolbar label.avatar-color-0 { background-color: #1a73e8; }
        .kite-toolbar label.avatar-color-1 { background-color: #d93025; }
        .kite-toolbar label.avatar-color-2 { background-color: #2e9e60; }
        .kite-toolbar label.avatar-color-3 { background-color: #d98a10; }
        .kite-toolbar label.avatar-color-4 { background-color: #8c59db; }
        .kite-toolbar label.avatar-color-5 { background-color: #0d9eaa; }
        .kite-toolbar label.avatar-color-6 { background-color: #e65a8b; }
        .kite-toolbar label.avatar-color-7 { background-color: #737d89; }
        .kite-toolbar label.avatar-guest { background-color: #6b7280; }

        /* Command palette */
        window.kite-command-window {
            background-color: #ffffff;
            border: 1px solid rgba(0, 0, 0, 0.12);
            border-radius: 14px;
        }
        .kite-command-window .kite-command-entry {
            min-height: 34px;
            padding: 6px 12px;
            border: 1px solid #dadce0;
            border-radius: 10px;
            background-image: none;
            background-color: #f8f9fa;
            color: #202124;
            box-shadow: none;
        }
        .kite-command-window .kite-command-entry:focus {
            border-color: #1a73e8;
            box-shadow: 0 0 0 2px rgba(26, 115, 232, 0.18);
        }
        .kite-command-window .command-list { background-color: transparent; }
        .kite-command-window .command-list row {
            padding: 2px 6px;
            border-radius: 8px;
            margin: 1px 8px;
        }
        .kite-command-window .command-list row:selected { background-color: rgba(26, 115, 232, 0.12); }
        .kite-command-window .command-title { color: #202124; font-size: 13.5px; font-weight: 600; }
        .kite-command-window .command-detail { color: #5f6368; font-size: 12px; }
        .kite-command-window .command-kind {
            color: #5f6368;
            font-size: 10px;
            font-weight: 700;
            letter-spacing: .07em;
        }
        .kite-command-window .command-footer { color: #80868b; font-size: 11.5px; }
        window.kite-command-window.kite-dark {
            background-color: #1f2023;
            border-color: #3c4043;
        }
        window.kite-command-window.kite-dark .kite-command-entry {
            background-color: #292a2d;
            color: #e8eaed;
            border-color: #3c4043;
        }
        window.kite-command-window.kite-dark .kite-command-entry:focus {
            border-color: #8ab4f8;
            box-shadow: 0 0 0 2px rgba(138, 180, 248, 0.20);
        }
        window.kite-command-window.kite-dark .command-list row:selected {
            background-color: rgba(138, 180, 248, 0.16);
        }
        window.kite-command-window.kite-dark .command-title { color: #e8eaed; }
        window.kite-command-window.kite-dark .command-detail,
        window.kite-command-window.kite-dark .command-kind,
        window.kite-command-window.kite-dark .command-footer { color: #9aa0a6; }
    )CSS";

    GtkCssProvider *css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css_provider, BROWSER_CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css_provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    init_adblocker();
    tracking_protection_init();
    init_dark_mode();

    app_context_init();
    WebKitWebContext *context = app_webkit_context();
    webkit_web_context_set_cache_model(context, WEBKIT_CACHE_MODEL_WEB_BROWSER);
    privacy_apply_context(context);
    init_kite_pages(context);
    init_downloads(context);
    extensions_init(context);

    g_unix_signal_add(SIGTERM, on_quit_signal, nullptr);
    g_unix_signal_add(SIGINT, on_quit_signal, nullptr);

    create_browser_window(startup_url);
    g_free(startup_url);
    gtk_main();

    privacy_clear_on_exit();
    history_close();
    if (profile_is_guest())
        profile_cleanup_guest();
    return 0;
}
