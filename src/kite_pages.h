#pragma once

#include <webkit2/webkit2.h>

#define KITE_NEWTAB_URL "kite://newtab"
#define KITE_HISTORY_URL "kite://history"
#define KITE_BOOKMARKS_URL "kite://bookmarks"

// Must be called on a context before any WebKitWebView is created with it.
void init_kite_pages(WebKitWebContext *context);
