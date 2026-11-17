#pragma once

#include <webkit2/webkit2.h>

WebKitUserContentManager *adblock_content_manager();
WebKitUserContentManager *adblock_private_content_manager();

// Adds or removes a browser-owned user script on both the normal and the
// private (incognito) content manager.
void adblock_add_script_to_all(WebKitUserScript *script);
void adblock_remove_script_from_all(WebKitUserScript *script);

void init_adblocker();

// Applies the global ad-block setting and rebuilds per-site exceptions.
void adblock_set_enabled(bool enabled);
void adblock_reload_exceptions();
