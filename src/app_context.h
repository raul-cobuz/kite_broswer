#pragma once

#include <webkit2/webkit2.h>

// Creates the WebKit context used by normal tabs. The default profile keeps the
// legacy default context; named profiles get an isolated website data manager.
void app_context_init();

WebKitWebContext *app_webkit_context();
bool app_context_is_isolated();
