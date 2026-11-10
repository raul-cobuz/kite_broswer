#include "tracking_protection.h"

#include <glib.h>
#include <webkit2/webkit2.h>

#include "adblock.h"
#include "settings.h"

// Keep the tracking-parameter names in sync with is_tracking_param() in
// url_utils.cpp. Link clicks are rewritten before the default navigation runs.
static const char tracking_script[] =
    "(function() {\n"
    "  if (window.__kiteTrackingGuard) return;\n"
    "  window.__kiteTrackingGuard = true;\n"
    "  var names = new Set([\n"
    "    'fbclid', 'gclid', 'dclid', 'gclsrc', 'wbraid', 'gbraid', 'gad_source',\n"
    "    'msclkid', 'mc_cid', 'mc_eid', 'mkt_tok', 'igshid', 'twclid', 'ttclid',\n"
    "    'yclid', 'srsltid', '_hsenc', '_hsmi', '__hssc', '__hstc', '__hsfp',\n"
    "    'oly_anon_id', 'oly_enc_id', 'vero_id', 'vero_conv', 'wickedid', '_ga',\n"
    "    '_gl', 'ref_src', 'ref_url', '__twitter_impression', 'guccounter',\n"
    "    'guce_referrer', 'guce_referrer_sig', 'spm', 'openstat', '_openstat',\n"
    "    'pk_campaign', 'pk_kwd', 'pk_source', 'pk_medium'\n"
    "  ]);\n"
    "  var prefixes = ['utm_', 'hsa_', 'mtm_'];\n"
    "  function isTracking(name) {\n"
    "    var lower = name.toLowerCase();\n"
    "    if (names.has(lower)) return true;\n"
    "    return prefixes.some(function(prefix) { return lower.indexOf(prefix) === 0; });\n"
    "  }\n"
    "  function clean(value) {\n"
    "    try {\n"
    "      var url = new URL(value, document.baseURI);\n"
    "      if (url.protocol !== 'http:' && url.protocol !== 'https:') return null;\n"
    "      var removed = false;\n"
    "      Array.from(url.searchParams.keys()).forEach(function(name) {\n"
    "        if (isTracking(name)) {\n"
    "          url.searchParams.delete(name);\n"
    "          removed = true;\n"
    "        }\n"
    "      });\n"
    "      return removed ? url.href : null;\n"
    "    } catch (error) {\n"
    "      return null;\n"
    "    }\n"
    "  }\n"
    "  function cleanAnchors(root) {\n"
    "    var links = root.querySelectorAll ? root.querySelectorAll('a[href]') : [];\n"
    "    Array.prototype.forEach.call(links, function(link) {\n"
    "      var cleaned = clean(link.href);\n"
    "      if (cleaned) link.setAttribute('href', cleaned);\n"
    "    });\n"
    "  }\n"
    "  document.addEventListener('click', function(event) {\n"
    "    if (event.defaultPrevented || event.button !== 0) return;\n"
    "    var element = event.target;\n"
    "    while (element && element.tagName !== 'A') element = element.parentElement;\n"
    "    if (!element || !element.href) return;\n"
    "    var cleaned = clean(element.href);\n"
    "    if (cleaned) element.setAttribute('href', cleaned);\n"
    "  }, true);\n"
    "  if (document.readyState === 'loading')\n"
    "    document.addEventListener('DOMContentLoaded', function() { cleanAnchors(document); });\n"
    "  else\n"
    "    cleanAnchors(document);\n"
    "})();\n";

static WebKitUserScript *tracking_script_handle = nullptr;

static void add_tracking_script() {
    if (tracking_script_handle)
        return;
    WebKitUserContentManager *manager = adblock_content_manager();
    if (!manager)
        return;
    tracking_script_handle = webkit_user_script_new(
        tracking_script, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END, nullptr,
        nullptr);
    webkit_user_content_manager_add_script(manager, tracking_script_handle);
}

static void remove_tracking_script() {
    if (!tracking_script_handle)
        return;
    WebKitUserContentManager *manager = adblock_content_manager();
    if (manager)
        webkit_user_content_manager_remove_script(manager, tracking_script_handle);
    webkit_user_script_unref(tracking_script_handle);
    tracking_script_handle = nullptr;
}

void tracking_protection_init() {
    if (settings_strip_tracking_params())
        add_tracking_script();
}

void tracking_protection_set_enabled(bool enabled) {
    if (enabled)
        add_tracking_script();
    else
        remove_tracking_script();
}
