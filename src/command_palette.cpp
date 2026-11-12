#include "command_palette.h"

#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

#include "browser_window.h"
#include "dark_mode.h"
#include "find_bar.h"
#include "kite_urls.h"
#include "omnibox.h"
#include "page_tools.h"
#include "settings.h"
#include "tabs.h"

namespace {

enum class CommandKind {
    Action,
    Url,
    Tab,
};

struct CommandItem {
    CommandKind kind = CommandKind::Action;
    std::string id;
    std::string title;
    std::string detail;
    std::string url;
    int tab_id = 0;
};

struct PaletteWidgets {
    GtkWidget *window = nullptr;
    GtkWidget *entry = nullptr;
    GtkWidget *list = nullptr;
};

PaletteWidgets palette;

std::string lower_text(const std::string &text) {
    gchar *value = g_ascii_strdown(text.c_str(), -1);
    std::string result = value ? value : "";
    g_free(value);
    return result;
}

int match_score(const std::string &text, const std::string &needle, int prefix_score, int contains_score) {
    if (needle.empty())
        return contains_score;
    std::string haystack = lower_text(text);
    if (haystack.rfind(needle, 0) == 0)
        return prefix_score;
    if (haystack.find(needle) != std::string::npos)
        return contains_score;
    return 0;
}

void add_action(std::vector<CommandItem> &items, const char *id, const char *title, const char *detail) {
    CommandItem item;
    item.kind = CommandKind::Action;
    item.id = id;
    item.title = title;
    item.detail = detail;
    items.push_back(item);
}

std::vector<CommandItem> action_items() {
    std::vector<CommandItem> items;
    add_action(items, "new-tab", "New tab", "Ctrl+T");
    add_action(items, "new-incognito", "New incognito tab", "Ctrl+Shift+P");
    add_action(items, "close-tab", "Close current tab", "Ctrl+W");
    add_action(items, "reopen-tab", "Reopen closed tab", "Ctrl+Shift+T");
    add_action(items, "reload", "Reload page", "Ctrl+R");
    add_action(items, "find", "Find in page", "Ctrl+F");
    add_action(items, "split", "Toggle split view", "Ctrl+\\");
    add_action(items, "dark-mode", "Toggle dark mode", "F9");
    add_action(items, "print", "Print page", "Ctrl+P");
    add_action(items, "screenshot-visible", "Screenshot visible area", "Save PNG");
    add_action(items, "screenshot-full", "Screenshot full page", "Save PNG");
    add_action(items, "history", "Open history", "Ctrl+H");
    add_action(items, "bookmarks", "Open bookmarks", "Ctrl+Shift+O");
    add_action(items, "settings", "Open settings", "Search and start page");
    add_action(items, "extensions", "Open extensions", "Manage extensions");
    add_action(items, "profiles", "Open profiles", "Switch or create a profile");
    add_action(items, "sync", "Open sync", "Shared folder sync");
    add_action(items, "newtab-page", "Open the Kite start page", "New tab");
    return items;
}

void open_url(const std::string &url) {
    if (url.empty())
        return;
    WebKitWebView *web_view = get_active_webview();
    if (web_view)
        webkit_web_view_load_uri(web_view, url.c_str());
    else
        create_new_tab(url.c_str());
}

void execute_item(const CommandItem &item) {
    switch (item.kind) {
    case CommandKind::Tab:
        tab_activate_id(item.tab_id);
        return;
    case CommandKind::Url:
        open_url(item.url);
        return;
    case CommandKind::Action:
        break;
    }

    const std::string &id = item.id;
    if (id == "new-tab") {
        create_new_tab(KITE_NEWTAB_URL);
        focus_url_bar();
    } else if (id == "new-incognito") {
        create_incognito_tab(KITE_NEWTAB_URL);
        focus_url_bar();
    } else if (id == "close-tab") {
        close_current_tab();
    } else if (id == "reopen-tab") {
        reopen_last_closed_tab();
    } else if (id == "reload") {
        if (WebKitWebView *web_view = get_active_webview())
            webkit_web_view_reload(web_view);
    } else if (id == "find") {
        if (browser.find_box && browser.find_entry) {
            sync_find_bar(get_active_webview());
            gtk_widget_show(browser.find_box);
            gtk_widget_grab_focus(browser.find_entry);
            gtk_editable_select_region(GTK_EDITABLE(browser.find_entry), 0, -1);
            const gchar *query = gtk_entry_get_text(GTK_ENTRY(browser.find_entry));
            if (query && *query && get_active_webview())
                run_find(get_active_webview(), query);
        }
    } else if (id == "split") {
        toggle_split_view();
    } else if (id == "dark-mode") {
        toggle_dark_mode();
    } else if (id == "print") {
        print_active_page();
    } else if (id == "screenshot-visible") {
        screenshot_active_page(false);
    } else if (id == "screenshot-full") {
        screenshot_active_page(true);
    } else if (id == "history") {
        create_new_tab(KITE_HISTORY_URL);
    } else if (id == "bookmarks") {
        create_new_tab(KITE_BOOKMARKS_URL);
    } else if (id == "settings") {
        create_new_tab(KITE_SETTINGS_URL);
    } else if (id == "extensions") {
        create_new_tab(KITE_EXTENSIONS_URL);
    } else if (id == "profiles") {
        create_new_tab(KITE_PROFILES_URL);
    } else if (id == "sync") {
        create_new_tab(KITE_SYNC_URL);
    } else if (id == "newtab-page") {
        create_new_tab(KITE_NEWTAB_URL);
    }
}

std::vector<CommandItem> ranked_items(const std::string &query) {
    std::string needle = lower_text(query);
    WebKitWebView *active = get_active_webview();
    bool incognito = active && webview_is_incognito(active);
    std::vector<std::pair<int, CommandItem>> ranked;
    std::unordered_set<std::string> seen;

    auto add = [&](const CommandItem &item, int score) {
        std::string key;
        if (item.kind == CommandKind::Tab)
            key = "tab:" + std::to_string(item.tab_id);
        else if (item.kind == CommandKind::Url)
            key = "url:" + item.url;
        else
            key = "action:" + item.id;
        if (!seen.insert(key).second)
            return;
        ranked.push_back({score, item});
    };

    for (const auto &tab : tabs_list()) {
        std::string title = tab.title.empty() ? tab.url : tab.title;
        std::string haystack = title + " " + tab.url + " tab";
        int score = match_score(haystack, needle, 130, 95);
        if (score > 0) {
            CommandItem item;
            item.kind = CommandKind::Tab;
            item.tab_id = tab.id;
            item.title = title;
            item.detail = incognito ? "Open tab" : (tab.url.empty() ? "Open tab" : tab.url);
            add(item, score + (tab.active ? 8 : 0));
        }
    }

    for (const auto &action : action_items()) {
        std::string haystack = action.title + " " + action.detail + " action";
        int score = match_score(haystack, needle, 110, 70);
        if (score > 0)
            add(action, score);
    }

    if (!incognito && !needle.empty()) {
        for (const auto &suggestion : omnibox_suggestions(query.c_str(), true, 24)) {
            if (suggestion.kind == OmniboxKind::Search || suggestion.kind == OmniboxKind::Open)
                continue;
            CommandItem item;
            item.kind = CommandKind::Url;
            item.title = suggestion.title;
            item.url = suggestion.url;
            item.detail = std::string(omnibox_kind_label(suggestion.kind));
            if (!suggestion.url.empty() && suggestion.url != suggestion.title)
                item.detail += " · " + suggestion.url;
            add(item, 55 + match_score(item.title + " " + item.url, needle, 60, 35));
        }
    }

    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const std::pair<int, CommandItem> &a, const std::pair<int, CommandItem> &b) {
                         return a.first > b.first;
                     });

    std::vector<CommandItem> result;
    for (const auto &entry : ranked) {
        result.push_back(entry.second);
        if (result.size() >= 40)
            break;
    }
    return result;
}

void clear_list() {
    if (!palette.list)
        return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(palette.list));
    for (GList *child = children; child; child = child->next)
        gtk_widget_destroy(GTK_WIDGET(child->data));
    g_list_free(children);
}

const char *kind_label(CommandKind kind) {
    switch (kind) {
    case CommandKind::Action:
        return "Action";
    case CommandKind::Url:
        return "Go";
    case CommandKind::Tab:
        return "Tab";
    }
    return "";
}

void select_row(int delta) {
    if (!palette.list)
        return;
    GtkListBoxRow *current = gtk_list_box_get_selected_row(GTK_LIST_BOX(palette.list));
    gint index = current ? gtk_list_box_row_get_index(current) : -1;
    gint count = 0;
    GList *children = gtk_container_get_children(GTK_CONTAINER(palette.list));
    count = (gint)g_list_length(children);
    g_list_free(children);
    if (count == 0)
        return;
    index += delta;
    if (index < 0)
        index = count - 1;
    if (index >= count)
        index = 0;
    GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(palette.list), index);
    if (row) {
        gtk_list_box_select_row(GTK_LIST_BOX(palette.list), row);
        gtk_widget_grab_focus(GTK_WIDGET(row));
    }
}

void activate_selected() {
    if (!palette.list)
        return;
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(palette.list));
    if (!row) {
        row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(palette.list), 0);
        if (!row)
            return;
    }
    gtk_list_box_select_row(GTK_LIST_BOX(palette.list), row);
    g_signal_emit_by_name(palette.list, "row-activated", row);
}

void rebuild_list(const std::string &query) {
    if (!palette.list)
        return;
    clear_list();

    std::vector<CommandItem> items = ranked_items(query);
    for (const auto &item : items) {
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_widget_set_margin_start(box, 16);
        gtk_widget_set_margin_end(box, 16);
        gtk_widget_set_margin_top(box, 9);
        gtk_widget_set_margin_bottom(box, 9);

        GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget *title = gtk_label_new(item.title.c_str());
        gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
        gtk_style_context_add_class(gtk_widget_get_style_context(title), "command-title");
        GtkWidget *detail = gtk_label_new(item.detail.c_str());
        gtk_label_set_xalign(GTK_LABEL(detail), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(detail), PANGO_ELLIPSIZE_MIDDLE);
        gtk_style_context_add_class(gtk_widget_get_style_context(detail), "command-detail");
        gtk_box_pack_start(GTK_BOX(text), title, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(text), detail, FALSE, FALSE, 0);

        GtkWidget *kind = gtk_label_new(kind_label(item.kind));
        gtk_label_set_xalign(GTK_LABEL(kind), 1.0f);
        gtk_style_context_add_class(gtk_widget_get_style_context(kind), "command-kind");

        gtk_box_pack_start(GTK_BOX(box), text, TRUE, TRUE, 0);
        gtk_box_pack_end(GTK_BOX(box), kind, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(row), box);
        gtk_widget_show_all(row);

        g_object_set_data_full(G_OBJECT(row), "kite-command-item", new CommandItem(item),
                               [](gpointer data) { delete static_cast<CommandItem *>(data); });
        gtk_list_box_insert(GTK_LIST_BOX(palette.list), row, -1);
    }

    GtkListBoxRow *first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(palette.list), 0);
    if (first)
        gtk_list_box_select_row(GTK_LIST_BOX(palette.list), first);
}

void close_palette() {
    if (palette.window)
        gtk_widget_destroy(palette.window);
}

void on_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer user_data) {
    (void)list;
    (void)user_data;
    auto *item = static_cast<CommandItem *>(g_object_get_data(G_OBJECT(row), "kite-command-item"));
    if (!item)
        return;
    CommandItem copy = *item;
    close_palette();
    execute_item(copy);
}

void on_search_changed(GtkEditable *editable, gpointer user_data) {
    (void)user_data;
    rebuild_list(gtk_entry_get_text(GTK_ENTRY(editable)));
}

gboolean on_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
    (void)widget;
    (void)user_data;
    if (event->keyval == GDK_KEY_Escape) {
        close_palette();
        return TRUE;
    }
    if (event->keyval == GDK_KEY_Down || event->keyval == GDK_KEY_KP_Down) {
        select_row(1);
        return TRUE;
    }
    if (event->keyval == GDK_KEY_Up || event->keyval == GDK_KEY_KP_Up) {
        select_row(-1);
        return TRUE;
    }
    if (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter) {
        activate_selected();
        return TRUE;
    }
    return FALSE;
}

void on_palette_destroy(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    (void)user_data;
    palette.window = nullptr;
    palette.entry = nullptr;
    palette.list = nullptr;
}

} // namespace

void show_command_palette() {
    if (palette.window) {
        gtk_window_present(GTK_WINDOW(palette.window));
        gtk_widget_grab_focus(palette.entry);
        return;
    }

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    palette.window = window;
    gtk_window_set_title(GTK_WINDOW(window), "Kite — Search tabs and commands");
    gtk_window_set_default_size(GTK_WINDOW(window), 680, 480);
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER_ON_PARENT);
    if (browser.main_window)
        gtk_window_set_transient_for(GTK_WINDOW(window), GTK_WINDOW(browser.main_window));
    gtk_window_set_modal(GTK_WINDOW(window), TRUE);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(window), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(window), "kite-command-window");
    if (settings_dark_mode())
        gtk_style_context_add_class(gtk_widget_get_style_context(window), "kite-dark");
    g_signal_connect(window, "destroy", G_CALLBACK(on_palette_destroy), nullptr);
    g_signal_connect(window, "key-press-event", G_CALLBACK(on_key_press), nullptr);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), outer);

    GtkWidget *entry = gtk_search_entry_new();
    palette.entry = entry;
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Search open tabs, history, bookmarks, and actions…");
    gtk_widget_set_margin_start(entry, 14);
    gtk_widget_set_margin_end(entry, 14);
    gtk_widget_set_margin_top(entry, 14);
    gtk_widget_set_margin_bottom(entry, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(entry), "kite-command-entry");
    gtk_box_pack_start(GTK_BOX(outer), entry, FALSE, FALSE, 0);
    g_signal_connect(entry, "changed", G_CALLBACK(on_search_changed), nullptr);
    g_signal_connect(entry, "key-press-event", G_CALLBACK(on_key_press), nullptr);

    GtkWidget *scroller = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_box_pack_start(GTK_BOX(outer), scroller, TRUE, TRUE, 0);

    GtkWidget *list = gtk_list_box_new();
    palette.list = list;
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(list), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(list), "command-list");
    g_signal_connect(list, "row-activated", G_CALLBACK(on_row_activated), nullptr);
    gtk_container_add(GTK_CONTAINER(scroller), list);

    GtkWidget *footer = gtk_label_new("↑ ↓ navigate   ·   Enter open   ·   Esc close");
    gtk_style_context_add_class(gtk_widget_get_style_context(footer), "command-footer");
    gtk_widget_set_margin_start(footer, 16);
    gtk_widget_set_margin_end(footer, 16);
    gtk_widget_set_margin_top(footer, 8);
    gtk_widget_set_margin_bottom(footer, 12);
    gtk_label_set_xalign(GTK_LABEL(footer), 0.0f);
    gtk_box_pack_start(GTK_BOX(outer), footer, FALSE, FALSE, 0);

    gtk_widget_show_all(window);
    gtk_widget_grab_focus(entry);
    rebuild_list("");
}
