#pragma once

#include <webkit2/webkit2.h>

WebKitUserContentManager *adblock_content_manager();
void init_adblocker();

// Applies the global ad-block setting and rebuilds per-site exceptions.
void adblock_set_enabled(bool enabled);
void adblock_reload_exceptions();
