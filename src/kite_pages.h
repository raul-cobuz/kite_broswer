#pragma once

#include <webkit2/webkit2.h>

#include "kite_urls.h"

// Must be called on a context before any WebKitWebView is created with it.
void init_kite_pages(WebKitWebContext *context);
