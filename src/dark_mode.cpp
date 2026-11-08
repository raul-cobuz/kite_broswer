#include "dark_mode.h"

#include "adblock.h"
#include "browser_window.h"
#include "settings.h"

static const gchar *DARK_MODE_STYLE_JS =
    "(function(){var id='kite-dark-mode-style';"
    "if(document.getElementById(id))return;"
    "var style=document.createElement('style');style.id=id;"
    "style.textContent='html { filter: invert(90%) hue-rotate(180deg) !important; background-color: #121212 !important; } "
    "img, video, canvas, [style*=\"background-image\"] { filter: invert(111%) hue-rotate(180deg) !important; }';"
    "(document.head||document.documentElement).appendChild(style);})();";

static const gchar *DARK_MODE_REMOVE_JS =
    "var el=document.getElementById('kite-dark-mode-style'); if (el) el.remove();";

static WebKitUserScript *dark_mode_script = nullptr;
static bool dark_script_added = false;

static void set_webview_dark_mode(WebKitWebView *web_view, bool enable) {
    webkit_web_view_evaluate_javascript(web_view, enable ? DARK_MODE_STYLE_JS : DARK_MODE_REMOVE_JS, -1, NULL, NULL,
                                        NULL, NULL, NULL);
}

static void set_dark_mode_script(bool enable) {
    WebKitUserContentManager *manager = adblock_content_manager();
    if (!manager || dark_script_added == enable)
        return;

    if (enable) {
        if (!dark_mode_script)
            dark_mode_script = webkit_user_script_new(DARK_MODE_STYLE_JS, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                                                      WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END, NULL, NULL);
        webkit_user_content_manager_add_script(manager, dark_mode_script);
    } else if (dark_mode_script) {
        webkit_user_content_manager_remove_script(manager, dark_mode_script);
    }
    dark_script_added = enable;
}

void apply_dark_mode(bool enable) {
    settings_set_dark_mode(enable);
    set_dark_mode_script(enable);

    gint n_pages = gtk_notebook_get_n_pages(GTK_NOTEBOOK(browser.notebook));
    for (gint i = 0; i < n_pages; i++) {
        GtkWidget *child = gtk_notebook_get_nth_page(GTK_NOTEBOOK(browser.notebook), i);
        set_webview_dark_mode(WEBKIT_WEB_VIEW(child), enable);
    }
    save_settings();
}

void init_dark_mode() {
    if (!dark_mode_script)
        dark_mode_script = webkit_user_script_new(DARK_MODE_STYLE_JS, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                                                  WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END, NULL, NULL);
    if (settings_dark_mode())
        set_dark_mode_script(true);
}

void on_dark_mode_toggled(GtkToggleButton *button, gpointer user_data) {
    apply_dark_mode(gtk_toggle_button_get_active(button));
}

void toggle_dark_mode() {
    if (!browser.btn_dark) {
        apply_dark_mode(!settings_dark_mode());
        return;
    }
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(browser.btn_dark),
                                 !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(browser.btn_dark)));
}
