#include "browser_window.h"

#include <cairo.h>
#include <climits>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk/gdkkeysyms.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#include "command_palette.h"
#include "dark_mode.h"
#include "downloads.h"
#include "find_bar.h"
#include "history.h"
#include "kite_pages.h"
#include "omnibox.h"
#include "page_tools.h"
#include "privacy.h"
#include "profile.h"
#include "session.h"
#include "settings.h"
#include "tabs.h"
#include "url_utils.h"

BrowserState browser;

static GtkListStore *completion_store = nullptr;

static void clear_omnibox_completions() {
    if (completion_store)
        gtk_list_store_clear(completion_store);
}

static WebKitWebView *notebook_current_view(GtkWidget *notebook_widget) {
    if (!notebook_widget)
        return nullptr;
    gint page_num = gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook_widget));
    if (page_num < 0)
        return nullptr;
    GtkWidget *child = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook_widget), page_num);
    return child && WEBKIT_IS_WEB_VIEW(child) ? WEBKIT_WEB_VIEW(child) : nullptr;
}

GtkNotebook *notebook_for_webview(WebKitWebView *web_view) {
    if (!web_view)
        return nullptr;
    if (browser.notebook &&
        gtk_notebook_page_num(GTK_NOTEBOOK(browser.notebook), GTK_WIDGET(web_view)) >= 0)
        return GTK_NOTEBOOK(browser.notebook);
    if (browser.split_notebook &&
        gtk_notebook_page_num(GTK_NOTEBOOK(browser.split_notebook), GTK_WIDGET(web_view)) >= 0)
        return GTK_NOTEBOOK(browser.split_notebook);
    return nullptr;
}

bool split_view_active() {
    if (!browser.split_notebook || !gtk_widget_get_visible(browser.split_notebook))
        return false;
    return gtk_notebook_get_n_pages(GTK_NOTEBOOK(browser.split_notebook)) > 0;
}

WebKitWebView *get_active_webview() {
    if (!browser.notebook)
        return nullptr;

    if (split_view_active() && browser.split_focus_secondary) {
        WebKitWebView *secondary = notebook_current_view(browser.split_notebook);
        if (secondary)
            return secondary;
    }

    WebKitWebView *primary = notebook_current_view(browser.notebook);
    if (primary)
        return primary;
    return split_view_active() ? notebook_current_view(browser.split_notebook) : nullptr;
}

gboolean webview_is_incognito(WebKitWebView *web_view) {
    if (!web_view)
        return FALSE;
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(web_view), "is_incognito"));
}

static void update_url_entry_icon(const gchar *uri) {
    if (!browser.url_entry)
        return;

    const gchar *icon = nullptr;
    const gchar *tooltip = nullptr;
    if (uri && g_str_has_prefix(uri, "https://")) {
        icon = "channel-secure-symbolic";
        tooltip = "Secure connection";
    } else if (uri && g_str_has_prefix(uri, "http://")) {
        icon = "channel-insecure-symbolic";
        tooltip = "Connection is not secure";
    } else if (uri && g_str_has_prefix(uri, "file://")) {
        icon = "text-x-generic-symbolic";
        tooltip = "Local file";
    } else if (uri && g_str_has_prefix(uri, "kite://")) {
        icon = "applications-internet-symbolic";
        tooltip = "Kite page";
    }

    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(browser.url_entry), GTK_ENTRY_ICON_PRIMARY, icon);
    gtk_entry_set_icon_tooltip_text(GTK_ENTRY(browser.url_entry), GTK_ENTRY_ICON_PRIMARY, tooltip);
}

void set_url_entry_text(const gchar *text) {
    if (!browser.url_entry)
        return;
    const gchar *display = text;
    if (display && g_str_has_prefix(display, KITE_NEWTAB_URL))
        display = "";
    browser.updating_url_entry = true;
    gtk_entry_set_text(GTK_ENTRY(browser.url_entry), display ? display : "");
    browser.updating_url_entry = false;
    browser.url_entry_user_edited = false;
    update_url_entry_icon(text);
    clear_omnibox_completions();
}

static bool can_bookmark_uri(const gchar *uri) {
    return uri && (g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://") ||
                   g_str_has_prefix(uri, "file://"));
}

void update_bookmark_button(WebKitWebView *web_view) {
    if (!browser.btn_bookmark)
        return;
    if (web_view && web_view != get_active_webview())
        return;

    const gchar *uri = web_view ? webkit_web_view_get_uri(web_view) : nullptr;
    gboolean incognito = webview_is_incognito(web_view);
    gboolean can_bookmark = can_bookmark_uri(uri) && !incognito;
    gboolean marked = can_bookmark && bookmark_exists(uri);

    browser.updating_bookmark_button = true;
    gtk_widget_set_sensitive(browser.btn_bookmark, can_bookmark);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(browser.btn_bookmark), marked);
    browser.updating_bookmark_button = false;

    gtk_button_set_image(GTK_BUTTON(browser.btn_bookmark),
                         gtk_image_new_from_icon_name(marked ? "starred-symbolic" : "non-starred-symbolic",
                                                      GTK_ICON_SIZE_BUTTON));
    if (incognito)
        gtk_widget_set_tooltip_text(browser.btn_bookmark, "Bookmarks are not saved in incognito");
    else if (marked)
        gtk_widget_set_tooltip_text(browser.btn_bookmark, "Remove bookmark (Ctrl+D)");
    else
        gtk_widget_set_tooltip_text(browser.btn_bookmark, "Bookmark this page (Ctrl+D)");
}

static bool is_startup_newtab(WebKitWebView *web_view) {
    if (!web_view || !g_object_get_data(G_OBJECT(web_view), "kite_startup_tab"))
        return false;
    const gchar *uri = webkit_web_view_get_uri(web_view);
    return uri && g_str_has_prefix(uri, KITE_NEWTAB_URL);
}

void save_browser_session() {
    if (!browser.notebook || browser.restoring_session || profile_is_guest())
        return;

    SessionState state;
    WebKitWebView *active = get_active_webview();

    auto append_notebook = [&](GtkWidget *notebook_widget) {
        if (!notebook_widget)
            return;
        gint n_pages = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook_widget));
        for (gint i = 0; i < n_pages; i++) {
            GtkWidget *child = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook_widget), i);
            if (!child || !WEBKIT_IS_WEB_VIEW(child))
                continue;
            WebKitWebView *web_view = WEBKIT_WEB_VIEW(child);
            if (webview_is_incognito(web_view))
                continue;
            // The fresh start page opened at launch is not part of the restored
            // session; otherwise every launch would accumulate another blank tab.
            if (is_startup_newtab(web_view))
                continue;

            if (web_view == active)
                state.active_index = (int)state.urls.size();

            const gchar *uri = webkit_web_view_get_uri(web_view);
            const gchar *requested = (const gchar *)g_object_get_data(G_OBJECT(web_view), "kite_requested_url");
            if (uri && *uri)
                state.urls.push_back(uri);
            else if (requested && *requested)
                state.urls.push_back(requested);
            else
                state.urls.push_back(KITE_NEWTAB_URL);
            state.pinned.push_back(tab_is_pinned(web_view));
        }
    };

    append_notebook(browser.notebook);
    append_notebook(browser.split_notebook);

    if (state.active_index >= (int)state.urls.size())
        state.active_index = 0;
    session_save(state);
}

void switch_profile_and_restart(const gchar *id) {
    if (!id || !*id)
        return;
    save_browser_session();
    save_settings();
    if (profile_switch(id))
        gtk_main_quit();
}

static void on_search_notice_response(GtkInfoBar *bar, gint response_id, gpointer user_data) {
    (void)user_data;
    const gchar *query = (const gchar *)g_object_get_data(G_OBJECT(bar), "kite_query");
    WebKitWebView *web_view = get_active_webview();

    if (response_id == GTK_RESPONSE_APPLY && query && *query && web_view) {
        std::string url = google_search_url(query);
        webkit_web_view_load_uri(web_view, url.c_str());
    } else if (response_id == GTK_RESPONSE_YES) {
        settings_set_search_engine("duckduckgo");
        save_settings();
        refresh_search_placeholder();
        if (query && *query && web_view) {
            std::string url = search_url_for_engine(query, "duckduckgo");
            webkit_web_view_load_uri(web_view, url.c_str());
        }
    }
    gtk_widget_hide(GTK_WIDGET(bar));
}

void show_google_fallback_notice(const gchar *query) {
    if (!browser.search_notice)
        return;

    GtkWidget *content = gtk_info_bar_get_content_area(GTK_INFO_BAR(browser.search_notice));
    GList *children = gtk_container_get_children(GTK_CONTAINER(content));
    GtkWidget *label = children ? GTK_WIDGET(children->data) : nullptr;
    if (label) {
        gchar *text = g_strdup_printf("Google blocked this search, so Kite loaded DuckDuckGo results for \"%s\".",
                                      query && *query ? query : "your query");
        gtk_label_set_text(GTK_LABEL(label), text);
        g_free(text);
    }
    g_list_free(children);

    g_object_set_data_full(G_OBJECT(browser.search_notice), "kite_query", g_strdup(query ? query : ""), g_free);
    gtk_info_bar_set_message_type(GTK_INFO_BAR(browser.search_notice), GTK_MESSAGE_WARNING);
    gtk_widget_show_all(browser.search_notice);
}

void refresh_search_placeholder() {
    if (!browser.url_entry)
        return;
    gchar *placeholder =
        g_strdup_printf("Search with %s or type a URL", search_engine_display_name(settings_search_engine()));
    gtk_entry_set_placeholder_text(GTK_ENTRY(browser.url_entry), placeholder);
    g_free(placeholder);
}

static void split_view_sync_button();
static gboolean center_split_pane(gpointer data);
static void on_split_toggled(GtkToggleButton *button, gpointer user_data);

// Refresh every toolbar element that reflects the focused tab. Keeping this in
// one place means the split panes can hand focus back and forth without
// duplicating the notebook switch logic.
static void update_active_view_ui(WebKitWebView *web_view) {
    if (!web_view)
        return;

    const gchar *uri = webkit_web_view_get_uri(web_view);
    set_url_entry_text(uri);
    clear_omnibox_completions();

    if (browser.url_entry) {
        GtkStyleContext *context = gtk_widget_get_style_context(browser.url_entry);
        if (webview_is_incognito(web_view))
            gtk_style_context_add_class(context, "incognito-mode");
        else
            gtk_style_context_remove_class(context, "incognito-mode");
    }

    update_reload_button(web_view);
    update_progress(web_view);
    update_window_title(web_view);
    update_bookmark_button(web_view);
    update_privacy_button(web_view);
    sync_find_bar(web_view);
}

static void split_view_sync_button() {
    if (!browser.btn_split)
        return;

    bool active = split_view_active();
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(browser.btn_split)) != active) {
        g_signal_handlers_block_by_func(browser.btn_split, (gpointer)on_split_toggled, nullptr);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(browser.btn_split), active);
        g_signal_handlers_unblock_by_func(browser.btn_split, (gpointer)on_split_toggled, nullptr);
    }
    gtk_widget_set_tooltip_text(browser.btn_split,
                                active ? "Close split view (Ctrl+\\)" : "Split view (Ctrl+\\)");
}

static gboolean center_split_pane(gpointer data) {
    (void)data;
    if (!browser.split_pane || !split_view_active())
        return G_SOURCE_REMOVE;
    gint width = gtk_widget_get_allocated_width(browser.split_pane);
    if (width > 0)
        gtk_paned_set_position(GTK_PANED(browser.split_pane), width / 2);
    return G_SOURCE_REMOVE;
}

// Move a tab (web view plus its tab label/buttons) between the notebooks.
// GTK owns the page while it is attached, so hold temporary references across
// remove/insert to keep the widget alive. The caller decides whether the moved
// page becomes current in the destination notebook.
static void move_notebook_page(GtkNotebook *from, GtkNotebook *to, WebKitWebView *web_view, gint position,
                               bool select) {
    if (!from || !to || !web_view)
        return;
    GtkWidget *child = GTK_WIDGET(web_view);
    gint page = gtk_notebook_page_num(from, child);
    if (page < 0)
        return;

    GtkWidget *tab_label = gtk_notebook_get_tab_label(from, child);
    g_object_ref(child);
    if (tab_label)
        g_object_ref(tab_label);

    gtk_notebook_remove_page(from, page);
    gint index = position < 0 ? gtk_notebook_append_page(to, child, tab_label)
                              : gtk_notebook_insert_page(to, child, tab_label, position);
    gtk_notebook_set_tab_reorderable(to, child, to == GTK_NOTEBOOK(browser.notebook));
    if (select)
        gtk_notebook_set_current_page(to, index);

    if (tab_label)
        g_object_unref(tab_label);
    g_object_unref(child);
}

static void deactivate_split_view() {
    if (!browser.split_notebook)
        return;
    gtk_widget_hide(browser.split_notebook);
    browser.split_focus_secondary = false;
    split_view_sync_button();
}

static void move_all_split_pages_to_main(gint insert_at) {
    if (!browser.notebook || !browser.split_notebook)
        return;
    GtkNotebook *main_notebook = GTK_NOTEBOOK(browser.notebook);
    GtkNotebook *split_notebook = GTK_NOTEBOOK(browser.split_notebook);
    if (insert_at < 0)
        insert_at = gtk_notebook_get_n_pages(main_notebook);

    while (gtk_notebook_get_n_pages(split_notebook) > 0) {
        GtkWidget *child = gtk_notebook_get_nth_page(split_notebook, 0);
        if (!child || !WEBKIT_IS_WEB_VIEW(child)) {
            gtk_notebook_remove_page(split_notebook, 0);
            continue;
        }
        move_notebook_page(split_notebook, main_notebook, WEBKIT_WEB_VIEW(child), insert_at++, false);
    }
}

void toggle_split_view() {
    if (!browser.notebook || !browser.split_notebook)
        return;

    if (split_view_active()) {
        GtkNotebook *main_notebook = GTK_NOTEBOOK(browser.notebook);
        gint insert_at = gtk_notebook_get_current_page(main_notebook) + 1;

        // Hide first so the toolbar button and get_active_webview() already see
        // a single pane while the tab moves back.
        deactivate_split_view();
        move_all_split_pages_to_main(insert_at);

        update_active_view_ui(get_active_webview());
        save_browser_session();
        return;
    }

    GtkNotebook *main_notebook = GTK_NOTEBOOK(browser.notebook);
    if (gtk_notebook_get_n_pages(main_notebook) == 0)
        create_new_tab(KITE_NEWTAB_URL);

    WebKitWebView *secondary = nullptr;
    if (gtk_notebook_get_n_pages(main_notebook) < 2) {
        // A single tab gets a fresh companion instead of duplicating the page.
        create_new_tab(KITE_NEWTAB_URL);
        secondary = get_active_webview();
    } else {
        gint current = gtk_notebook_get_current_page(main_notebook);
        gint pages = gtk_notebook_get_n_pages(main_notebook);
        GtkWidget *child = gtk_notebook_get_nth_page(main_notebook, (current + 1) % pages);
        if (child && WEBKIT_IS_WEB_VIEW(child))
            secondary = WEBKIT_WEB_VIEW(child);
    }
    if (!secondary) {
        split_view_sync_button();
        return;
    }

    move_notebook_page(main_notebook, GTK_NOTEBOOK(browser.split_notebook), secondary, -1, true);
    gtk_widget_show(browser.split_notebook);
    gtk_widget_show(browser.split_pane);
    g_idle_add(center_split_pane, nullptr);

    // The tab the user came from keeps focus and the address bar.
    browser.split_focus_secondary = false;
    split_view_sync_button();
    update_active_view_ui(get_active_webview());
    save_browser_session();
}

void move_tab_to_split(WebKitWebView *web_view) {
    if (!web_view || !browser.notebook || !browser.split_notebook)
        return;
    GtkNotebook *notebook = notebook_for_webview(web_view);
    if (!notebook)
        return;

    GtkNotebook *main_notebook = GTK_NOTEBOOK(browser.notebook);
    GtkNotebook *split_notebook = GTK_NOTEBOOK(browser.split_notebook);

    if (notebook == split_notebook) {
        gint insert_at = gtk_notebook_get_current_page(main_notebook) + 1;
        move_notebook_page(split_notebook, main_notebook, web_view, insert_at, true);
        browser.split_focus_secondary = false;
        if (gtk_notebook_get_n_pages(split_notebook) == 0)
            deactivate_split_view();
        activate_webview(web_view);
        save_browser_session();
        return;
    }

    // A split needs a companion, otherwise moving the only tab would leave the
    // primary pane empty.
    if (gtk_notebook_get_n_pages(main_notebook) < 2)
        create_new_tab(KITE_NEWTAB_URL);

    gtk_widget_show(browser.split_notebook);
    gtk_widget_show(browser.split_pane);
    move_notebook_page(main_notebook, split_notebook, web_view, -1, true);
    g_idle_add(center_split_pane, nullptr);
    browser.split_focus_secondary = true;
    split_view_sync_button();
    update_active_view_ui(web_view);
    save_browser_session();
}

static void on_split_toggled(GtkToggleButton *button, gpointer user_data) {
    (void)user_data;
    bool want_active = gtk_toggle_button_get_active(button);
    if (want_active == split_view_active())
        return;
    toggle_split_view();
}

bool activate_webview(WebKitWebView *web_view) {
    GtkNotebook *notebook = notebook_for_webview(web_view);
    if (!notebook)
        return false;

    gint page = gtk_notebook_page_num(notebook, GTK_WIDGET(web_view));
    if (page < 0)
        return false;

    gtk_notebook_set_current_page(notebook, page);
    browser.split_focus_secondary =
        split_view_active() && notebook == GTK_NOTEBOOK(browser.split_notebook);
    update_active_view_ui(web_view);
    gtk_widget_grab_focus(GTK_WIDGET(web_view));
    return true;
}

void split_view_note_focus(WebKitWebView *web_view) {
    if (!web_view || !split_view_active())
        return;
    GtkNotebook *notebook = notebook_for_webview(web_view);
    if (!notebook)
        return;
    browser.split_focus_secondary = notebook == GTK_NOTEBOOK(browser.split_notebook);
    update_active_view_ui(web_view);
}

void split_view_after_tab_removed(GtkNotebook *notebook) {
    if (!browser.notebook || !browser.split_notebook)
        return;

    GtkNotebook *main_notebook = GTK_NOTEBOOK(browser.notebook);
    GtkNotebook *split_notebook = GTK_NOTEBOOK(browser.split_notebook);

    if (notebook == split_notebook) {
        if (gtk_notebook_get_n_pages(split_notebook) == 0) {
            deactivate_split_view();
            update_active_view_ui(get_active_webview());
        }
        return;
    }

    if (notebook == main_notebook && gtk_notebook_get_n_pages(main_notebook) == 0) {
        if (split_view_active()) {
            deactivate_split_view();
            move_all_split_pages_to_main(0);
            update_active_view_ui(get_active_webview());
        }
    }
}

static void cycle_active_tab(bool backward) {
    std::vector<WebKitWebView *> views;
    auto collect = [&](GtkWidget *notebook_widget) {
        if (!notebook_widget)
            return;
        gint pages = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook_widget));
        for (gint i = 0; i < pages; i++) {
            GtkWidget *child = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook_widget), i);
            if (child && WEBKIT_IS_WEB_VIEW(child))
                views.push_back(WEBKIT_WEB_VIEW(child));
        }
    };
    collect(browser.notebook);
    if (split_view_active())
        collect(browser.split_notebook);
    if (views.empty())
        return;

    WebKitWebView *active = get_active_webview();
    auto found = std::find(views.begin(), views.end(), active);
    size_t index = found == views.end() ? 0 : (size_t)std::distance(views.begin(), found);
    size_t next = backward ? (index + views.size() - 1) % views.size() : (index + 1) % views.size();
    activate_webview(views[next]);
}

static void on_url_entry_changed(GtkEditable *editable, gpointer user_data) {
    if (browser.updating_url_entry)
        return;

    browser.url_entry_user_edited = true;
    if (!completion_store)
        return;

    const gchar *text = gtk_entry_get_text(GTK_ENTRY(browser.url_entry));
    gtk_list_store_clear(completion_store);
    if (!text || !*text)
        return;

    WebKitWebView *web_view = get_active_webview();
    bool incognito = webview_is_incognito(web_view) != FALSE;
    std::vector<OmniboxSuggestion> matches = omnibox_suggestions(text, !incognito, 12);
    for (const auto &entry : matches) {
        GtkTreeIter iter;
        std::string display;
        if (entry.kind == OmniboxKind::Search || entry.kind == OmniboxKind::Open) {
            display = entry.title;
        } else {
            display = entry.title;
            if (entry.kind == OmniboxKind::Pin)
                display = "Pinned · " + display;
            else if (entry.kind == OmniboxKind::Bookmark)
                display = "Bookmark · " + display;
            else if (entry.kind == OmniboxKind::Page)
                display = "Kite · " + display;
            if (!entry.url.empty() && entry.url != entry.title)
                display += " \xE2\x80\x94 " + entry.url;
        }
        gtk_list_store_append(completion_store, &iter);
        gtk_list_store_set(completion_store, &iter, 0, display.c_str(), 1, entry.url.c_str(), -1);
    }
}

static void navigate_url_entry(const gchar *text) {
    WebKitWebView *web_view = get_active_webview();
    if (!web_view || !text || !*text)
        return;

    std::string target_url = resolve_url_input(text);
    g_printerr("[Kite DEBUG] navigate_url_entry text=%s -> target=%s\n", text, target_url.c_str());
    webkit_web_view_load_uri(web_view, target_url.c_str());
    gtk_widget_grab_focus(GTK_WIDGET(web_view));
}

static void on_url_activate(GtkEntry *entry, gpointer user_data) {
    navigate_url_entry(gtk_entry_get_text(entry));
}

static gboolean on_completion_match(GtkEntryCompletion *completion, GtkTreeModel *model, GtkTreeIter *iter,
                                    gpointer user_data) {
    gchar *url = nullptr;
    gtk_tree_model_get(model, iter, 1, &url, -1);
    if (url && *url) {
        set_url_entry_text(url);
        WebKitWebView *web_view = get_active_webview();
        if (web_view) {
            webkit_web_view_load_uri(web_view, url);
            gtk_widget_grab_focus(GTK_WIDGET(web_view));
        }
    }
    g_free(url);
    return TRUE;
}

static gboolean completion_match_always(GtkEntryCompletion *completion, const gchar *key, GtkTreeIter *iter,
                                        gpointer user_data) {
    return TRUE;
}

static void setup_omnibox_completion() {
    completion_store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    GtkEntryCompletion *completion = gtk_entry_completion_new();
    gtk_entry_completion_set_model(completion, GTK_TREE_MODEL(completion_store));
    gtk_entry_completion_set_text_column(completion, 0);
    gtk_entry_completion_set_popup_completion(completion, TRUE);
    gtk_entry_completion_set_inline_completion(completion, FALSE);
    gtk_entry_completion_set_popup_set_width(completion, TRUE);
    gtk_entry_completion_set_minimum_key_length(completion, 1);
    gtk_entry_completion_set_match_func(completion, completion_match_always, nullptr, nullptr);
    g_signal_connect(completion, "match-selected", G_CALLBACK(on_completion_match), nullptr);
    gtk_entry_set_completion(GTK_ENTRY(browser.url_entry), completion);
}

void update_reload_button(WebKitWebView *web_view) {
    if (!browser.btn_reload || !web_view)
        return;
    gboolean loading = webkit_web_view_is_loading(web_view);
    GtkWidget *image = gtk_image_new_from_icon_name(loading ? "process-stop-symbolic" : "view-refresh-symbolic",
                                                   GTK_ICON_SIZE_BUTTON);
    gtk_button_set_image(GTK_BUTTON(browser.btn_reload), image);
    gtk_widget_set_tooltip_text(browser.btn_reload, loading ? "Stop loading" : "Reload");
}

void update_window_title(WebKitWebView *web_view) {
    if (!browser.main_window)
        return;

    auto title_for = [](WebKitWebView *view) -> std::string {
        if (!view)
            return "";
        const gchar *title = webkit_web_view_get_title(view);
        if (!title || !*title)
            title = webkit_web_view_get_uri(view);
        return title && *title ? title : "";
    };

    std::string title;
    if (split_view_active()) {
        std::string left = title_for(notebook_current_view(browser.notebook));
        std::string right = title_for(notebook_current_view(browser.split_notebook));
        if (!left.empty() && !right.empty())
            title = left + "  |  " + right;
        else
            title = left.empty() ? right : left;
    } else {
        title = title_for(web_view ? web_view : get_active_webview());
    }

    gtk_window_set_title(GTK_WINDOW(browser.main_window), title.empty() ? "Kite Browser" : title.c_str());
}

void update_progress(WebKitWebView *web_view) {
    if (!browser.progress_bar || !web_view)
        return;

    gboolean loading = webkit_web_view_is_loading(web_view);
    gtk_widget_set_visible(browser.progress_bar, loading);
    if (loading)
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(browser.progress_bar),
                                      webkit_web_view_get_estimated_load_progress(web_view));
}

void on_progress_changed(GObject *object, GParamSpec *pspec, gpointer user_data) {
    WebKitWebView *web_view = WEBKIT_WEB_VIEW(object);
    if (web_view != get_active_webview())
        return;
    if (browser.progress_bar && webkit_web_view_is_loading(web_view))
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(browser.progress_bar),
                                      webkit_web_view_get_estimated_load_progress(web_view));
}

void focus_url_bar() {
    if (!browser.url_entry)
        return;
    gtk_widget_grab_focus(browser.url_entry);
    gtk_editable_select_region(GTK_EDITABLE(browser.url_entry), 0, -1);
}

void close_current_tab() {
    WebKitWebView *web_view = get_active_webview();
    if (web_view)
        close_tab(web_view);
}

static void on_back_clicked(GtkButton *btn, gpointer user_data) {
    WebKitWebView *web_view = get_active_webview();
    if (web_view)
        webkit_web_view_go_back(web_view);
}

static void on_forward_clicked(GtkButton *btn, gpointer user_data) {
    WebKitWebView *web_view = get_active_webview();
    if (web_view)
        webkit_web_view_go_forward(web_view);
}

static void on_reload_clicked(GtkButton *btn, gpointer user_data) {
    WebKitWebView *web_view = get_active_webview();
    if (!web_view)
        return;
    if (webkit_web_view_is_loading(web_view))
        webkit_web_view_stop_loading(web_view);
    else
        webkit_web_view_reload(web_view);
}

static void on_home_clicked(GtkButton *btn, gpointer user_data) {
    WebKitWebView *web_view = get_active_webview();
    if (web_view)
        webkit_web_view_load_uri(web_view, settings_homepage());
}

static void on_new_tab_clicked(GtkButton *btn, gpointer user_data) {
    create_new_tab(KITE_NEWTAB_URL);
    focus_url_bar();
}

static void on_extensions_clicked(GtkButton *btn, gpointer user_data) {
    WebKitWebView *web_view = get_active_webview();
    if (web_view)
        webkit_web_view_load_uri(web_view, KITE_EXTENSIONS_URL);
    else
        create_new_tab(KITE_EXTENSIONS_URL);
}

static void on_profile_clicked(GtkButton *btn, gpointer user_data) {
    WebKitWebView *web_view = get_active_webview();
    if (web_view)
        webkit_web_view_load_uri(web_view, KITE_PROFILES_URL);
    else
        create_new_tab(KITE_PROFILES_URL);
}

static void on_bookmark_toggled(GtkToggleButton *button, gpointer user_data) {
    if (browser.updating_bookmark_button)
        return;

    WebKitWebView *web_view = get_active_webview();
    const gchar *uri = web_view ? webkit_web_view_get_uri(web_view) : nullptr;
    if (!web_view || webview_is_incognito(web_view) || !can_bookmark_uri(uri)) {
        update_bookmark_button(web_view);
        return;
    }

    const gchar *title = webkit_web_view_get_title(web_view);
    if (gtk_toggle_button_get_active(button))
        bookmark_add(uri, title ? title : "");
    else
        bookmark_remove(uri);
    update_bookmark_button(web_view);
}

static void set_active_zoom(double level) {
    WebKitWebView *web_view = get_active_webview();
    if (!web_view)
        return;

    level = CLAMP(level, 0.25, 5.0);
    webkit_web_view_set_zoom_level(web_view, level);
    if (webview_is_incognito(web_view))
        return;

    const gchar *uri = webkit_web_view_get_uri(web_view);
    if (!can_bookmark_uri(uri))
        return;
    std::string host = url_host(uri);
    if (host.empty())
        return;

    if (level > 0.999 && level < 1.001)
        settings_clear_site_zoom(host.c_str());
    else
        settings_set_site_zoom(host.c_str(), level);
    save_settings();
}

static std::string avatar_color_class(const std::string &id) {
    int color = profile_avatar_color(id);
    return color < 0 ? "avatar-guest" : "avatar-color-" + std::to_string(color);
}

static const char *KITE_SVG_CONTENT =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"128\" height=\"128\" viewBox=\"0 0 128 128\">\n"
    "  <defs>\n"
    "    <linearGradient id=\"bg\" x1=\"18\" y1=\"12\" x2=\"110\" y2=\"118\" gradientUnits=\"userSpaceOnUse\">\n"
    "      <stop offset=\"0\" stop-color=\"#4f9cf9\"/>\n"
    "      <stop offset=\"1\" stop-color=\"#1a73e8\"/>\n"
    "    </linearGradient>\n"
    "  </defs>\n"
    "  <rect x=\"8\" y=\"8\" width=\"112\" height=\"112\" rx=\"26\" fill=\"url(#bg)\"/>\n"
    "  <path d=\"M64 26 94 58 64 102 34 58Z\" fill=\"#ffffff\"/>\n"
    "  <path d=\"M64 26 64 102 34 58Z\" fill=\"#dbe8fc\"/>\n"
    "  <path d=\"M64 58 94 58 64 102Z\" fill=\"#b9d2fa\"/>\n"
    "  <path d=\"M34 58 64 58 64 102Z\" fill=\"#eef4fe\"/>\n"
    "  <path d=\"M64 26 94 58 64 58Z\" fill=\"#f6f9ff\"/>\n"
    "  <path d=\"M64 102 88 121 48 121Z\" fill=\"#ffffff\" opacity=\".9\"/>\n"
    "</svg>\n";

static gchar *find_kite_svg_path() {
    if (g_file_test("data/kite.svg", G_FILE_TEST_IS_REGULAR))
        return g_strdup("data/kite.svg");
    if (g_file_test("kite.svg", G_FILE_TEST_IS_REGULAR))
        return g_strdup("kite.svg");

    gchar exe_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len > 0) {
        exe_path[len] = '\0';
        gchar *exe_dir = g_path_get_dirname(exe_path);
        const gchar *rel_candidates[] = {
            "data/kite.svg",
            "../data/kite.svg",
            "../../data/kite.svg",
            "../share/icons/hicolor/scalable/apps/kite.svg",
            "../share/icons/kite.svg",
            nullptr
        };
        for (int i = 0; rel_candidates[i]; i++) {
            gchar *candidate = g_build_filename(exe_dir, rel_candidates[i], nullptr);
            if (g_file_test(candidate, G_FILE_TEST_IS_REGULAR)) {
                g_free(exe_dir);
                return candidate;
            }
            g_free(candidate);
        }
        g_free(exe_dir);
    }
    gchar *user_icon = g_build_filename(g_get_user_data_dir(), "icons", "hicolor", "scalable", "apps", "kite.svg", nullptr);
    if (g_file_test(user_icon, G_FILE_TEST_IS_REGULAR))
        return user_icon;
    g_free(user_icon);

    gchar *user_direct_icon = g_build_filename(g_get_user_data_dir(), "icons", "kite.svg", nullptr);
    if (g_file_test(user_direct_icon, G_FILE_TEST_IS_REGULAR))
        return user_direct_icon;
    g_free(user_direct_icon);

    return nullptr;
}

static GdkPixbuf *create_kite_logo_pixbuf(int width, int height) {
    gchar *svg_path = find_kite_svg_path();
    GdkPixbuf *pb = nullptr;
    if (svg_path) {
        pb = gdk_pixbuf_new_from_file_at_scale(svg_path, width, height, TRUE, nullptr);
        g_free(svg_path);
    }
    if (!pb) {
        GdkPixbufLoader *loader = gdk_pixbuf_loader_new_with_type("svg", nullptr);
        if (loader) {
            gdk_pixbuf_loader_set_size(loader, width, height);
            gdk_pixbuf_loader_write(loader, (const guchar *)KITE_SVG_CONTENT, strlen(KITE_SVG_CONTENT), nullptr);
            gdk_pixbuf_loader_close(loader, nullptr);
            pb = gdk_pixbuf_loader_get_pixbuf(loader);
            if (pb)
                g_object_ref(pb);
            g_object_unref(loader);
        }
    }
    return pb;
}

static GList *create_kite_logo_icon_list() {
    static const int sizes[] = {16, 24, 32, 48, 64, 128, 256};
    GList *list = nullptr;
    for (int size : sizes) {
        GdkPixbuf *pb = create_kite_logo_pixbuf(size, size);
        if (pb)
            list = g_list_append(list, pb);
    }
    return list;
}

void setup_kite_window_icon(GtkWindow *window) {
    gchar *svg_path = find_kite_svg_path();
    if (svg_path) {
        gchar *svg_dir = g_path_get_dirname(svg_path);
        gtk_icon_theme_append_search_path(gtk_icon_theme_get_default(), svg_dir);
        g_free(svg_dir);
        g_free(svg_path);
    }

    GList *icons = create_kite_logo_icon_list();
    if (icons) {
        if (window)
            gtk_window_set_icon_list(window, icons);
        gtk_window_set_default_icon_list(icons);
        g_list_free_full(icons, g_object_unref);
    }
    if (window)
        gtk_window_set_icon_name(window, "kite");
    gtk_window_set_default_icon_name("kite");
}

static void on_palette_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    show_command_palette();
}

static void on_screenshot_visible(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    (void)user_data;
    screenshot_active_page(false);
}

static void on_screenshot_full(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    (void)user_data;
    screenshot_active_page(true);
}

static void on_print_menu(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    (void)user_data;
    print_active_page();
}

static void on_accel_palette(GtkAccelGroup *group, GObject *object, guint keyval,
                             GdkModifierType modifier, gpointer user_data) {
    (void)group;
    (void)object;
    (void)keyval;
    (void)modifier;
    (void)user_data;
    show_command_palette();
}

static void on_accel_print(GtkAccelGroup *group, GObject *object, guint keyval,
                           GdkModifierType modifier, gpointer user_data) {
    (void)group;
    (void)object;
    (void)keyval;
    (void)modifier;
    (void)user_data;
    print_active_page();
}

static void on_accel_screenshot_visible(GtkAccelGroup *group, GObject *object, guint keyval,
                                        GdkModifierType modifier, gpointer user_data) {
    (void)group;
    (void)object;
    (void)keyval;
    (void)modifier;
    (void)user_data;
    screenshot_active_page(false);
}

static void on_accel_screenshot_full(GtkAccelGroup *group, GObject *object, guint keyval,
                                     GdkModifierType modifier, gpointer user_data) {
    (void)group;
    (void)object;
    (void)keyval;
    (void)modifier;
    (void)user_data;
    screenshot_active_page(true);
}

static gboolean on_window_delete(GtkWidget *widget, GdkEvent *event, gpointer user_data) {
    save_browser_session();
    return FALSE;
}

static void on_page_reordered(GtkNotebook *notebook, GtkWidget *child, guint new_page_num, gpointer user_data) {
    (void)child;
    (void)new_page_num;
    (void)user_data;
    tabs_enforce_pinned_order(notebook);
    save_browser_session();
}

static void on_tab_switched(GtkNotebook *nb, GtkWidget *page, guint page_num, gpointer user_data) {
    (void)nb;
    (void)page_num;
    if (!page || !WEBKIT_IS_WEB_VIEW(page))
        return;
    WebKitWebView *web_view = WEBKIT_WEB_VIEW(page);

    bool secondary = GPOINTER_TO_INT(user_data) == 1;
    browser.split_focus_secondary = split_view_active() && secondary;
    if (browser.search_notice)
        gtk_widget_hide(browser.search_notice);
    update_active_view_ui(web_view);
    save_browser_session();
}

static gboolean on_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
    // F12 sau Ctrl+Shift+I pentru Web Inspector
    if (event->keyval == GDK_KEY_F12 ||
        ((event->state & GDK_CONTROL_MASK) && (event->state & GDK_SHIFT_MASK) &&
         (event->keyval == GDK_KEY_I || event->keyval == GDK_KEY_i))) {
        WebKitWebView *web_view = get_active_webview();
        if (web_view)
            webkit_web_inspector_show(webkit_web_view_get_inspector(web_view));
        return TRUE;
    }

    // Escape pentru ascunderea barei de Ctrl+F
    if (event->keyval == GDK_KEY_Escape && browser.find_box && gtk_widget_get_visible(browser.find_box)) {
        close_find_bar();
        return TRUE;
    }

    // F9 pentru Dark Mode
    if (event->keyval == GDK_KEY_F9) {
        toggle_dark_mode();
        return TRUE;
    }

    if (event->state & GDK_CONTROL_MASK) {
        // Ctrl+Tab / Ctrl+Shift+Tab — cycle every tab, including the split pane.
        if (event->keyval == GDK_KEY_Tab || event->keyval == GDK_KEY_ISO_Left_Tab) {
            gboolean backward =
                (event->state & GDK_SHIFT_MASK) || event->keyval == GDK_KEY_ISO_Left_Tab;
            cycle_active_tab(backward);
            return TRUE;
        }

        // Ctrl+\ — toggle side-by-side split view.
        if (event->keyval == GDK_KEY_backslash) {
            toggle_split_view();
            return TRUE;
        }

        // Ctrl + Shift + D pentru Dark Mode
        if ((event->state & GDK_SHIFT_MASK) && (event->keyval == GDK_KEY_D || event->keyval == GDK_KEY_d)) {
            toggle_dark_mode();
            return TRUE;
        }

        // Ctrl + Shift + P pentru Incognito
        if ((event->state & GDK_SHIFT_MASK) && (event->keyval == GDK_KEY_P || event->keyval == GDK_KEY_p)) {
            create_incognito_tab(KITE_NEWTAB_URL);
            focus_url_bar();
            return TRUE;
        }

        // Ctrl + Shift + T — reopen the most recently closed tab
        if ((event->state & GDK_SHIFT_MASK) && (event->keyval == GDK_KEY_T || event->keyval == GDK_KEY_t)) {
            reopen_last_closed_tab();
            focus_url_bar();
            return TRUE;
        }

        // Ctrl + Shift + O — bookmarks manager
        if ((event->state & GDK_SHIFT_MASK) && (event->keyval == GDK_KEY_O || event->keyval == GDK_KEY_o)) {
            create_new_tab(KITE_BOOKMARKS_URL);
            focus_url_bar();
            return TRUE;
        }

        // Ctrl+K / Ctrl+Shift+A — search tabs, history, bookmarks and actions.
        if (!(event->state & GDK_SHIFT_MASK) && (event->keyval == GDK_KEY_k || event->keyval == GDK_KEY_K)) {
            show_command_palette();
            return TRUE;
        }
        if ((event->state & GDK_SHIFT_MASK) && (event->keyval == GDK_KEY_A || event->keyval == GDK_KEY_a)) {
            show_command_palette();
            return TRUE;
        }

        // Ctrl+P — native GTK print dialog through WebKit.
        if (!(event->state & GDK_SHIFT_MASK) && (event->keyval == GDK_KEY_p || event->keyval == GDK_KEY_P)) {
            print_active_page();
            return TRUE;
        }

        // Ctrl+Shift+S / Ctrl+Shift+E — save visible or full-page screenshots.
        if ((event->state & GDK_SHIFT_MASK) && (event->keyval == GDK_KEY_S || event->keyval == GDK_KEY_s)) {
            screenshot_active_page(false);
            return TRUE;
        }
        if ((event->state & GDK_SHIFT_MASK) && (event->keyval == GDK_KEY_E || event->keyval == GDK_KEY_e)) {
            screenshot_active_page(true);
            return TRUE;
        }

        switch (event->keyval) {
            case GDK_KEY_f:
            case GDK_KEY_F:
                sync_find_bar(get_active_webview());
                gtk_widget_show(browser.find_box);
                gtk_widget_grab_focus(browser.find_entry);
                gtk_editable_select_region(GTK_EDITABLE(browser.find_entry), 0, -1);
                if (*gtk_entry_get_text(GTK_ENTRY(browser.find_entry)) && get_active_webview())
                    run_find(get_active_webview(), gtk_entry_get_text(GTK_ENTRY(browser.find_entry)));
                return TRUE;
            case GDK_KEY_t:
                create_new_tab(KITE_NEWTAB_URL);
                focus_url_bar();
                return TRUE;
            case GDK_KEY_h:
            case GDK_KEY_H:
                create_new_tab(KITE_HISTORY_URL);
                focus_url_bar();
                return TRUE;
            case GDK_KEY_d:
            case GDK_KEY_D:
                if (browser.btn_bookmark && gtk_widget_get_sensitive(browser.btn_bookmark))
                    gtk_button_clicked(GTK_BUTTON(browser.btn_bookmark));
                return TRUE;
            case GDK_KEY_w:
                close_current_tab();
                return TRUE;
            case GDK_KEY_l:
                focus_url_bar();
                return TRUE;
            case GDK_KEY_r:
            case GDK_KEY_R: {
                WebKitWebView *web_view = get_active_webview();
                if (web_view)
                    webkit_web_view_reload(web_view);
                return TRUE;
            }
            case GDK_KEY_plus:
            case GDK_KEY_equal: {
                WebKitWebView *web_view = get_active_webview();
                if (web_view)
                    set_active_zoom(webkit_web_view_get_zoom_level(web_view) + 0.1);
                return TRUE;
            }
            case GDK_KEY_minus: {
                WebKitWebView *web_view = get_active_webview();
                if (web_view)
                    set_active_zoom(webkit_web_view_get_zoom_level(web_view) - 0.1);
                return TRUE;
            }
            case GDK_KEY_0:
                set_active_zoom(1.0);
                return TRUE;
        }
    }
    return FALSE;
}

void create_browser_window(const gchar *startup_url) {
    browser.main_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    // Keep the WM_CLASS in sync with StartupWMClass in kite.desktop.
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gtk_window_set_wmclass(GTK_WINDOW(browser.main_window), "kite", "Kite");
    G_GNUC_END_IGNORE_DEPRECATIONS
    setup_kite_window_icon(GTK_WINDOW(browser.main_window));
    gtk_style_context_add_class(gtk_widget_get_style_context(browser.main_window), "kite-window");
    if (settings_dark_mode())
        gtk_style_context_add_class(gtk_widget_get_style_context(browser.main_window), "kite-dark");
    g_signal_connect(browser.main_window, "key-press-event", G_CALLBACK(on_key_press), NULL);
    g_signal_connect(browser.main_window, "delete-event", G_CALLBACK(on_window_delete), NULL);

    GtkAccelGroup *accels = gtk_accel_group_new();
    gtk_accel_group_connect(accels, GDK_KEY_k, GDK_CONTROL_MASK, (GtkAccelFlags)0,
                            g_cclosure_new(G_CALLBACK(on_accel_palette), nullptr, nullptr));
    gtk_accel_group_connect(accels, GDK_KEY_A,
                            (GdkModifierType)(GDK_CONTROL_MASK | GDK_SHIFT_MASK), (GtkAccelFlags)0,
                            g_cclosure_new(G_CALLBACK(on_accel_palette), nullptr, nullptr));
    gtk_accel_group_connect(accels, GDK_KEY_p, GDK_CONTROL_MASK, (GtkAccelFlags)0,
                            g_cclosure_new(G_CALLBACK(on_accel_print), nullptr, nullptr));
    gtk_accel_group_connect(accels, GDK_KEY_S, (GdkModifierType)(GDK_CONTROL_MASK | GDK_SHIFT_MASK),
                            (GtkAccelFlags)0,
                            g_cclosure_new(G_CALLBACK(on_accel_screenshot_visible), nullptr, nullptr));
    gtk_accel_group_connect(accels, GDK_KEY_E, (GdkModifierType)(GDK_CONTROL_MASK | GDK_SHIFT_MASK),
                            (GtkAccelFlags)0,
                            g_cclosure_new(G_CALLBACK(on_accel_screenshot_full), nullptr, nullptr));
    gtk_window_add_accel_group(GTK_WINDOW(browser.main_window), accels);
    gtk_window_set_default_size(GTK_WINDOW(browser.main_window), 1200, 800);
    gtk_window_set_title(GTK_WINDOW(browser.main_window), "Kite Browser");
    g_signal_connect(browser.main_window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(browser.main_window), vbox);

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_style_context_add_class(gtk_widget_get_style_context(toolbar), "kite-toolbar");
    gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 0);

    GtkWidget *btn_back = gtk_button_new_from_icon_name("go-previous-symbolic", GTK_ICON_SIZE_BUTTON);
    GtkWidget *btn_forward = gtk_button_new_from_icon_name("go-next-symbolic", GTK_ICON_SIZE_BUTTON);
    browser.btn_reload = gtk_button_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_always_show_image(GTK_BUTTON(browser.btn_reload), TRUE);
    GtkWidget *btn_home = gtk_button_new_from_icon_name("go-home-symbolic", GTK_ICON_SIZE_BUTTON);
    GtkWidget *btn_new_tab = gtk_button_new_from_icon_name("list-add-symbolic", GTK_ICON_SIZE_BUTTON);
    browser.btn_split = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(browser.btn_split),
                         gtk_image_new_from_icon_name("view-dual-symbolic", GTK_ICON_SIZE_BUTTON));
    GtkWidget *btn_extensions =
        gtk_button_new_from_icon_name("applications-other-symbolic", GTK_ICON_SIZE_BUTTON);
    GtkWidget *btn_palette = gtk_button_new_from_icon_name("system-search-symbolic", GTK_ICON_SIZE_BUTTON);

    GtkWidget *btn_profile = gtk_button_new();
    GtkWidget *profile_avatar = gtk_label_new(profile_avatar_text(profile_current_name()).c_str());
    gtk_style_context_add_class(gtk_widget_get_style_context(profile_avatar), "profile-avatar");
    gtk_style_context_add_class(gtk_widget_get_style_context(profile_avatar),
                                avatar_color_class(profile_current_id()).c_str());
    gtk_container_add(GTK_CONTAINER(btn_profile), profile_avatar);

    GtkWidget *btn_tools = gtk_menu_button_new();
    gtk_button_set_image(GTK_BUTTON(btn_tools),
                         gtk_image_new_from_icon_name("camera-photo-symbolic", GTK_ICON_SIZE_BUTTON));
    GtkWidget *tools_menu = gtk_menu_new();
    GtkWidget *shot_visible = gtk_menu_item_new_with_label("Screenshot visible area");
    GtkWidget *shot_full = gtk_menu_item_new_with_label("Screenshot full page");
    GtkWidget *print_item = gtk_menu_item_new_with_label("Print…");
    g_signal_connect(shot_visible, "activate", G_CALLBACK(on_screenshot_visible), nullptr);
    g_signal_connect(shot_full, "activate", G_CALLBACK(on_screenshot_full), nullptr);
    g_signal_connect(print_item, "activate", G_CALLBACK(on_print_menu), nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(tools_menu), shot_visible);
    gtk_menu_shell_append(GTK_MENU_SHELL(tools_menu), shot_full);
    gtk_menu_shell_append(GTK_MENU_SHELL(tools_menu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(tools_menu), print_item);
    gtk_widget_show_all(tools_menu);
    gtk_menu_button_set_popup(GTK_MENU_BUTTON(btn_tools), tools_menu);

    browser.btn_bookmark = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(browser.btn_bookmark),
                         gtk_image_new_from_icon_name("non-starred-symbolic", GTK_ICON_SIZE_BUTTON));
    browser.btn_dark = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(browser.btn_dark),
                         gtk_image_new_from_icon_name("weather-clear-night-symbolic", GTK_ICON_SIZE_BUTTON));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(browser.btn_dark), settings_dark_mode());

    gtk_widget_set_tooltip_text(browser.btn_reload, "Reload");
    gtk_widget_set_tooltip_text(btn_home, "Home");
    gtk_widget_set_tooltip_text(btn_new_tab, "New tab (Ctrl+T)");
    gtk_widget_set_tooltip_text(browser.btn_split, "Split view (Ctrl+\\)");
    gtk_widget_set_tooltip_text(btn_extensions, "Extensions");
    gtk_widget_set_tooltip_text(btn_palette, "Search tabs and commands (Ctrl+K)");
    gtk_widget_set_tooltip_text(btn_profile, (std::string("Profiles — ") + profile_current_name()).c_str());
    gtk_widget_set_tooltip_text(btn_tools, "Screenshot and print");
    gtk_widget_set_tooltip_text(browser.btn_bookmark, "Bookmark this page (Ctrl+D)");
    gtk_widget_set_tooltip_text(browser.btn_dark, "Toggle Dark Mode (F9 / Ctrl+Shift+D)");

    g_signal_connect(btn_back, "clicked", G_CALLBACK(on_back_clicked), NULL);
    g_signal_connect(btn_forward, "clicked", G_CALLBACK(on_forward_clicked), NULL);
    g_signal_connect(browser.btn_reload, "clicked", G_CALLBACK(on_reload_clicked), NULL);
    g_signal_connect(btn_home, "clicked", G_CALLBACK(on_home_clicked), NULL);
    g_signal_connect(btn_new_tab, "clicked", G_CALLBACK(on_new_tab_clicked), NULL);
    g_signal_connect(browser.btn_split, "toggled", G_CALLBACK(on_split_toggled), NULL);
    g_signal_connect(btn_extensions, "clicked", G_CALLBACK(on_extensions_clicked), NULL);
    g_signal_connect(btn_palette, "clicked", G_CALLBACK(on_palette_clicked), NULL);
    g_signal_connect(btn_profile, "clicked", G_CALLBACK(on_profile_clicked), NULL);
    g_signal_connect(browser.btn_bookmark, "toggled", G_CALLBACK(on_bookmark_toggled), NULL);
    g_signal_connect(browser.btn_dark, "toggled", G_CALLBACK(on_dark_mode_toggled), NULL);

    gtk_box_pack_start(GTK_BOX(toolbar), btn_back, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_forward, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), browser.btn_reload, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_home, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_new_tab, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), browser.btn_split, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_extensions, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_palette, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_profile, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), browser.btn_bookmark, FALSE, FALSE, 0);

    browser.url_entry = gtk_entry_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(browser.url_entry), "kite-omnibox");
    refresh_search_placeholder();
    gtk_box_pack_start(GTK_BOX(toolbar), browser.url_entry, TRUE, TRUE, 2);
    g_signal_connect(browser.url_entry, "activate", G_CALLBACK(on_url_activate), NULL);
    g_signal_connect(browser.url_entry, "changed", G_CALLBACK(on_url_entry_changed), NULL);
    setup_omnibox_completion();

    GtkWidget *btn_privacy = create_privacy_button();
    if (btn_privacy)
        gtk_box_pack_start(GTK_BOX(toolbar), btn_privacy, FALSE, FALSE, 0);

    GtkWidget *btn_downloads = create_downloads_button();
    if (btn_downloads)
        gtk_box_pack_start(GTK_BOX(toolbar), btn_downloads, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_tools, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), browser.btn_dark, FALSE, FALSE, 0);

    browser.search_notice = gtk_info_bar_new();
    gtk_info_bar_set_message_type(GTK_INFO_BAR(browser.search_notice), GTK_MESSAGE_WARNING);
    GtkWidget *notice_content = gtk_info_bar_get_content_area(GTK_INFO_BAR(browser.search_notice));
    GtkWidget *notice_label = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(notice_label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(notice_label), 0.0);
    gtk_container_add(GTK_CONTAINER(notice_content), notice_label);
    gtk_info_bar_add_button(GTK_INFO_BAR(browser.search_notice), "Retry Google", GTK_RESPONSE_APPLY);
    gtk_info_bar_add_button(GTK_INFO_BAR(browser.search_notice), "Always use DuckDuckGo", GTK_RESPONSE_YES);
    gtk_info_bar_add_button(GTK_INFO_BAR(browser.search_notice), "Dismiss", GTK_RESPONSE_CLOSE);
    g_signal_connect(browser.search_notice, "response", G_CALLBACK(on_search_notice_response), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), browser.search_notice, FALSE, FALSE, 0);

    browser.progress_bar = gtk_progress_bar_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(browser.progress_bar), "kite-progress");
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(browser.progress_bar), FALSE);
    gtk_widget_set_size_request(browser.progress_bar, -1, 3);
    gtk_box_pack_start(GTK_BOX(vbox), browser.progress_bar, FALSE, FALSE, 0);

    browser.split_pane = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_style_context_add_class(gtk_widget_get_style_context(browser.split_pane), "kite-split-pane");
    gtk_paned_set_wide_handle(GTK_PANED(browser.split_pane), FALSE);
    gtk_box_pack_start(GTK_BOX(vbox), browser.split_pane, TRUE, TRUE, 0);

    browser.notebook = gtk_notebook_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(browser.notebook), "kite-notebook");
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(browser.notebook), TRUE);
    gtk_paned_pack1(GTK_PANED(browser.split_pane), browser.notebook, TRUE, FALSE);
    g_signal_connect(browser.notebook, "switch-page", G_CALLBACK(on_tab_switched), GINT_TO_POINTER(0));
    g_signal_connect(browser.notebook, "page-reordered", G_CALLBACK(on_page_reordered), NULL);
    tabs_install_notebook_context_menu(GTK_NOTEBOOK(browser.notebook));

    browser.split_notebook = gtk_notebook_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(browser.split_notebook), "kite-notebook");
    gtk_style_context_add_class(gtk_widget_get_style_context(browser.split_notebook), "kite-split-notebook");
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(browser.split_notebook), TRUE);
    gtk_widget_set_no_show_all(browser.split_notebook, TRUE);
    gtk_paned_pack2(GTK_PANED(browser.split_pane), browser.split_notebook, TRUE, FALSE);
    g_signal_connect(browser.split_notebook, "switch-page", G_CALLBACK(on_tab_switched),
                     GINT_TO_POINTER(1));
    g_signal_connect(browser.split_notebook, "page-reordered", G_CALLBACK(on_page_reordered), NULL);
    tabs_install_notebook_context_menu(GTK_NOTEBOOK(browser.split_notebook));

    GtkWidget *find_box = create_find_bar();
    gtk_box_pack_start(GTK_BOX(vbox), find_box, FALSE, FALSE, 2);

    browser.restoring_session = true;
    SessionState saved;
    bool restored = !profile_is_guest() && settings_restore_session() && session_load(&saved);
    if (restored) {
        for (size_t i = 0; i < saved.urls.size(); i++) {
            create_new_tab(saved.urls[i].c_str());
            if (i < saved.pinned.size() && saved.pinned[i]) {
                WebKitWebView *restored_view = get_active_webview();
                if (restored_view)
                    tab_set_pinned(restored_view, true);
            }
        }
    }

    // Always open the Kite start page. When a session was restored it becomes
    // the first, active tab, so the browser opens with a proper start page
    // instead of dropping straight into the last visited site.
    std::string initial_url = startup_url && *startup_url ? resolve_url_input(startup_url) : KITE_NEWTAB_URL;
    create_new_tab(initial_url.c_str());
    WebKitWebView *start_view = get_active_webview();
    if (start_view) {
        g_object_set_data(G_OBJECT(start_view), "kite_startup_tab", GINT_TO_POINTER(1));
        if (restored) {
            // Keep pinned restored tabs at the far left. The start page becomes
            // the first normal tab and stays active.
            gint pinned_count = 0;
            gint pages = gtk_notebook_get_n_pages(GTK_NOTEBOOK(browser.notebook));
            for (gint i = 0; i < pages; i++) {
                GtkWidget *child = gtk_notebook_get_nth_page(GTK_NOTEBOOK(browser.notebook), i);
                if (child && WEBKIT_IS_WEB_VIEW(child) && tab_is_pinned(WEBKIT_WEB_VIEW(child)))
                    pinned_count++;
            }
            gint start_page = gtk_notebook_page_num(GTK_NOTEBOOK(browser.notebook), GTK_WIDGET(start_view));
            if (start_page != pinned_count)
                gtk_notebook_reorder_child(GTK_NOTEBOOK(browser.notebook), GTK_WIDGET(start_view), pinned_count);
            gtk_notebook_set_current_page(GTK_NOTEBOOK(browser.notebook), pinned_count);
        }
    }
    browser.restoring_session = false;

    gtk_widget_show_all(browser.main_window);
    gtk_widget_hide(browser.split_notebook);
    gtk_widget_hide(browser.find_box);
    gtk_widget_hide(browser.progress_bar);
    gtk_widget_hide(browser.search_notice);
    focus_url_bar();
}
