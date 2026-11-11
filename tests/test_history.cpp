#include "history.h"

#include <glib.h>
#include <glib/gstdio.h>

#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;

static void check(bool condition, const char *label) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", label);
        failures++;
    }
}

static const HistoryEntry *find_entry(const std::vector<HistoryEntry> &entries, const std::string &url) {
    for (const auto &entry : entries) {
        if (entry.url == url)
            return &entry;
    }
    return nullptr;
}

int main() {
    GError *error = nullptr;
    gchar *tmp = g_dir_make_tmp("kite-history-XXXXXX", &error);
    if (!tmp) {
        std::fprintf(stderr, "Could not create temp dir: %s\n", error ? error->message : "unknown error");
        if (error)
            g_error_free(error);
        return 1;
    }
    gchar *db_path = g_build_filename(tmp, "kite.db", NULL);

    check(history_init(db_path), "database opens");
    check(history_is_ready(), "database is ready");
    check(history_recent(10).empty(), "new database is empty");
    check(bookmarks_list().empty(), "new bookmark list is empty");

    history_record_visit("https://example.com/a", "");
    history_record_visit("https://example.com/a", "");
    history_record_visit("https://other.test/page", "Other page");

    std::vector<HistoryEntry> entries = history_recent(10);
    check(entries.size() == 2, "two visits recorded");
    const HistoryEntry *example = find_entry(entries, "https://example.com/a");
    check(example != nullptr, "example visit present");
    if (example) {
        check(example->visit_count == 2, "repeat visit increments count");
        check(example->host == "example.com", "visit host recorded");
        check(example->title.empty(), "commit visit has no title yet");
    }

    history_update_title("https://example.com/a", "Example title");
    entries = history_search("example title", 10);
    check(entries.size() == 1, "search finds updated title");
    if (!entries.empty())
        check(entries[0].title == "Example title", "title updated");

    entries = history_search("other", 10);
    check(entries.size() == 1, "search by URL text");
    if (!entries.empty())
        check(entries[0].url == "https://other.test/page", "search returns the right URL");

    entries = history_completions("exam", 10);
    check(find_entry(entries, "https://example.com/a") != nullptr, "completion matches URL");

    check(!bookmark_exists("https://example.com/a"), "page not bookmarked yet");
    check(bookmark_add("https://example.com/a", "Example title"), "bookmark added");
    check(bookmark_exists("https://example.com/a"), "bookmark exists");
    entries = bookmarks_list();
    check(entries.size() == 1, "bookmark listed");
    if (!entries.empty())
        check(entries[0].title == "Example title", "bookmark title stored");

    bool marked = false;
    check(bookmark_toggle("https://example.com/a", "Example title", &marked), "bookmark toggled");
    check(!marked && !bookmark_exists("https://example.com/a"), "toggle removes bookmark");
    check(bookmark_toggle("https://example.com/a", "Example title", &marked), "bookmark toggled back");
    check(marked && bookmark_exists("https://example.com/a"), "toggle adds bookmark");

    entries = history_completions("example title", 10);
    check(find_entry(entries, "https://example.com/a") != nullptr, "completion includes bookmarks");

    check(pinned_links_list().empty(), "new pinned list is empty");
    check(pinned_links_count() == 0, "pinned count starts at zero");
    for (int i = 1; i <= KITE_MAX_PINNED_LINKS; i++) {
        gchar *url = g_strdup_printf("https://pin%d.test/", i);
        gchar *title = g_strdup_printf("Pin %d", i);
        check(pinned_link_add(url, title), "pinned link added");
        g_free(url);
        g_free(title);
    }
    check(pinned_links_count() == KITE_MAX_PINNED_LINKS, "pinned count reaches the limit");
    check(!pinned_link_add("https://pin-overflow.test/", "Overflow"), "sixth pinned link rejected");
    std::vector<PinnedLink> pins = pinned_links_list();
    check(pins.size() == (size_t)KITE_MAX_PINNED_LINKS, "pinned list respects the limit");
    if (!pins.empty()) {
        check(pins.front().url == "https://pin1.test/", "pinned links keep insertion order");
        check(pins.front().title == "Pin 1", "pinned title stored");
    }
    check(pinned_link_add("https://pin1.test/", "Pin One Updated"), "existing pinned link updates");
    pins = pinned_links_list();
    check(pins.size() == (size_t)KITE_MAX_PINNED_LINKS, "updating does not add another pin");
    if (!pins.empty())
        check(pins.front().title == "Pin One Updated", "pinned title updated");
    check(pinned_link_remove("https://pin2.test/"), "pinned link removed");
    check(pinned_links_count() == KITE_MAX_PINNED_LINKS - 1, "pinned count drops after removal");
    check(pinned_link_add("https://pin-overflow.test/", "Overflow"), "freed slot can be reused");

    history_delete_url("https://other.test/page");
    entries = history_recent(10);
    check(entries.size() == 1, "single URL forgotten");
    check(find_entry(entries, "https://other.test/page") == nullptr, "forgotten URL is gone");

    history_clear();
    check(history_recent(10).empty(), "history cleared");
    check(bookmark_exists("https://example.com/a"), "clearing history keeps bookmarks");
    check(pinned_links_count() == KITE_MAX_PINNED_LINKS, "clearing history keeps pinned links");

    history_record_visit("https://persisted.test/", "Persisted");
    check(bookmark_add("https://saved.test/", "Saved"), "second bookmark added");
    history_close();

    check(history_init(db_path), "database reopens");
    entries = history_recent(10);
    check(find_entry(entries, "https://persisted.test/") != nullptr, "history survives reopen");
    check(bookmark_exists("https://saved.test/"), "bookmark survives reopen");
    check(bookmark_exists("https://example.com/a"), "bookmark kept for bookmarked URL");
    check(pinned_links_count() == KITE_MAX_PINNED_LINKS, "pinned links survive reopen");
    history_close();

    g_remove(db_path);
    g_free(db_path);
    g_rmdir(tmp);
    g_free(tmp);

    if (failures) {
        std::fprintf(stderr, "%d history test(s) failed\n", failures);
        return 1;
    }
    std::printf("history tests passed\n");
    return 0;
}
