#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

#include "adblock.h"
#include "browser_window.h"
#include "dark_mode.h"
#include "downloads.h"
#include "extensions.h"
#include "history.h"
#include "kite_pages.h"
#include "privacy.h"
#include "settings.h"
#include "tracking_protection.h"

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);
    g_set_application_name("Kite Browser");

    load_settings();
    save_settings();
    history_init();

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
    )CSS";

    GtkCssProvider *css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css_provider, BROWSER_CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css_provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    init_adblocker();
    extensions_init();
    tracking_protection_init();
    init_dark_mode();

    WebKitWebContext *context = webkit_web_context_get_default();
    webkit_web_context_set_cache_model(context, WEBKIT_CACHE_MODEL_WEB_BROWSER);
    privacy_apply_context(context);
    init_kite_pages(context);
    init_downloads();

    create_browser_window();
    gtk_main();

    privacy_clear_on_exit();
    history_close();
    return 0;
}
