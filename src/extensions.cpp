#include "extensions.h"

#include <glib/gstdio.h>
#include <jsc/jsc.h>
#include <webkit2/webkit2.h>

#include <algorithm>
#include <cerrno>
#include <string>
#include <vector>

#include "adblock.h"
#include "app_context.h"
#include "browser_window.h"
#include "profile.h"
#include "tabs.h"

namespace {

struct ContentScriptSpec {
    std::vector<std::string> matches;
    std::vector<std::string> js;
    std::vector<std::string> css;
    std::string run_at;
    bool all_frames = false;
};

struct DispatchRequest {
    std::string request_id;
    std::string message_json;
    std::string sender_json;
};

struct BackgroundHost {
    std::string extension_id;
    std::string handler_name;
    std::string install_reason;
    GtkWidget *window = nullptr;
    WebKitWebView *web_view = nullptr;
    WebKitUserContentManager *manager = nullptr;
    bool loaded = false;
    bool announced = false;
    std::vector<DispatchRequest> queued;
};

struct RegisteredHandler {
    std::string name;
    std::string world;
    gulong signal_id = 0;
};

struct PendingReply {
    WebKitScriptMessageReply *reply = nullptr;
    JSCContext *context = nullptr;
    std::string extension_id;
    guint timeout_id = 0;
};

gchar *extension_root = nullptr;
WebKitWebContext *web_context = nullptr;
std::vector<WebKitUserScript *> injected_scripts;
std::vector<WebKitUserStyleSheet *> injected_styles;
std::vector<ExtensionInfo> loaded_extensions;
std::vector<BackgroundHost *> background_hosts;
std::vector<RegisteredHandler> content_handlers;
GHashTable *pending_replies = nullptr;
guint request_counter = 0;

void on_script_message(WebKitUserContentManager *manager, JSCValue *value, WebKitScriptMessageReply *reply,
                       gpointer user_data);

// ---------------------------------------------------------------------------
// JSON helpers

std::string json_string(JSCValue *object, const char *property) {
    if (!object)
        return "";
    JSCValue *value = jsc_value_object_get_property(object, property);
    std::string result;
    if (value && jsc_value_is_string(value)) {
        gchar *text = jsc_value_to_string(value);
        result = text ? text : "";
        g_free(text);
    }
    if (value)
        g_object_unref(value);
    return result;
}

bool json_bool(JSCValue *object, const char *property) {
    if (!object)
        return false;
    JSCValue *value = jsc_value_object_get_property(object, property);
    bool result = value && jsc_value_is_boolean(value) && jsc_value_to_boolean(value);
    if (value)
        g_object_unref(value);
    return result;
}

int json_int(JSCValue *object, const char *property) {
    if (!object)
        return 0;
    JSCValue *value = jsc_value_object_get_property(object, property);
    int result = value && jsc_value_is_number(value) ? jsc_value_to_int32(value) : 0;
    if (value)
        g_object_unref(value);
    return result;
}

std::vector<std::string> json_string_array(JSCValue *object, const char *property) {
    std::vector<std::string> result;
    if (!object)
        return result;

    JSCValue *array = jsc_value_object_get_property(object, property);
    if (!array || !jsc_value_is_array(array)) {
        if (array)
            g_object_unref(array);
        return result;
    }

    JSCValue *length_value = jsc_value_object_get_property(array, "length");
    int length = length_value ? jsc_value_to_int32(length_value) : 0;
    if (length_value)
        g_object_unref(length_value);

    for (int i = 0; i < length; i++) {
        JSCValue *item = jsc_value_object_get_property_at_index(array, (guint)i);
        if (item && jsc_value_is_string(item)) {
            gchar *text = jsc_value_to_string(item);
            if (text && *text)
                result.push_back(text);
            g_free(text);
        }
        if (item)
            g_object_unref(item);
    }
    g_object_unref(array);
    return result;
}

std::string value_to_json(JSCValue *value) {
    if (!value || jsc_value_is_undefined(value))
        return "null";
    gchar *json = jsc_value_to_json(value, 0);
    std::string result = json ? json : "null";
    g_free(json);
    return result;
}

std::string json_escape(const std::string &text) {
    GString *out = g_string_new(nullptr);
    for (unsigned char c : text) {
        switch (c) {
        case '"':
            g_string_append(out, "\\\"");
            break;
        case '\\':
            g_string_append(out, "\\\\");
            break;
        case '\n':
            g_string_append(out, "\\n");
            break;
        case '\r':
            g_string_append(out, "\\r");
            break;
        case '\t':
            g_string_append(out, "\\t");
            break;
        default:
            if (c < 0x20)
                g_string_append_printf(out, "\\u%04x", c);
            else
                g_string_append_c(out, (gchar)c);
            break;
        }
    }
    std::string result = out->str;
    g_string_free(out, TRUE);
    return result;
}

std::string js_string(const std::string &text) {
    return "\"" + json_escape(text) + "\"";
}

std::string error_reply(const std::string &message) {
    return "{\"ok\":false,\"error\":" + js_string(message) + "}";
}

std::string ok_reply(const std::string &value_json) {
    return "{\"ok\":true,\"value\":" + (value_json.empty() ? "null" : value_json) + "}";
}

std::string json_error(JSCContext *context) {
    JSCException *exception = jsc_context_get_exception(context);
    if (!exception)
        return "invalid JSON";
    const char *message = jsc_exception_get_message(exception);
    std::string result = message && *message ? message : "invalid JSON";
    jsc_context_clear_exception(context);
    return result;
}

bool parse_manifest(const gchar *manifest_path, ExtensionInfo &info, std::vector<ContentScriptSpec> &scripts) {
    gchar *contents = nullptr;
    gsize length = 0;
    GError *error = nullptr;
    if (!g_file_get_contents(manifest_path, &contents, &length, &error)) {
        info.error = error ? error->message : "Could not read manifest.json";
        g_clear_error(&error);
        return false;
    }
    info.manifest_json.assign(contents, length);

    JSCContext *context = jsc_context_new();
    JSCValue *root = jsc_value_new_from_json(context, contents);
    g_free(contents);

    if (!root || !jsc_value_is_object(root)) {
        info.error = "Invalid manifest.json: " + json_error(context);
        if (root)
            g_object_unref(root);
        g_object_unref(context);
        return false;
    }

    info.name = json_string(root, "name");
    info.version = json_string(root, "version");
    info.description = json_string(root, "description");
    info.permissions = json_string_array(root, "permissions");
    info.host_permissions = json_string_array(root, "host_permissions");

    JSCValue *background = jsc_value_object_get_property(root, "background");
    if (background && jsc_value_is_object(background)) {
        info.background_scripts = json_string_array(background, "scripts");
        info.background_page = json_string(background, "page");
        if (info.background_scripts.empty()) {
            std::string worker = json_string(background, "service_worker");
            if (!worker.empty())
                info.background_scripts.push_back(worker);
        }
    }
    if (background)
        g_object_unref(background);

    JSCValue *content_scripts = jsc_value_object_get_property(root, "content_scripts");
    if (content_scripts && jsc_value_is_array(content_scripts)) {
        JSCValue *length_value = jsc_value_object_get_property(content_scripts, "length");
        int count = length_value ? jsc_value_to_int32(length_value) : 0;
        if (length_value)
            g_object_unref(length_value);

        for (int i = 0; i < count; i++) {
            JSCValue *entry = jsc_value_object_get_property_at_index(content_scripts, (guint)i);
            if (!entry || !jsc_value_is_object(entry)) {
                if (entry)
                    g_object_unref(entry);
                continue;
            }

            ContentScriptSpec spec;
            spec.matches = json_string_array(entry, "matches");
            spec.js = json_string_array(entry, "js");
            spec.css = json_string_array(entry, "css");
            spec.run_at = json_string(entry, "run_at");
            spec.all_frames = json_bool(entry, "all_frames");
            g_object_unref(entry);

            info.script_count += (int)spec.js.size();
            info.style_count += (int)spec.css.size();
            if (!spec.js.empty() || !spec.css.empty())
                scripts.push_back(spec);
        }
    }
    if (content_scripts)
        g_object_unref(content_scripts);

    g_object_unref(root);
    g_object_unref(context);

    if (info.name.empty())
        info.name = "Unnamed extension";
    return true;
}

// ---------------------------------------------------------------------------
// Paths and files

std::string sanitize_identifier(const gchar *raw) {
    std::string result;
    for (const gchar *p = raw; p && *p; p++) {
        guchar c = (guchar)*p;
        if (g_ascii_isalnum(c) || c == '-' || c == '_' || c == '.') {
            result.push_back((char)g_ascii_tolower(c));
        } else if (!result.empty() && result.back() != '-') {
            result.push_back('-');
        }
    }
    while (!result.empty() && (result.back() == '-' || result.back() == '.'))
        result.pop_back();
    return result;
}

bool valid_identifier(const std::string &id) {
    if (id.empty() || id == "." || id == "..")
        return false;
    for (unsigned char c : id) {
        if (!(g_ascii_isalnum(c) || c == '-' || c == '_' || c == '.'))
            return false;
    }
    return true;
}

gchar *extension_path(const std::string &id) {
    return g_build_filename(extensions_directory(), id.c_str(), nullptr);
}

std::string world_name_for(const std::string &id) {
    std::string world = "kite_ext_";
    for (unsigned char c : id)
        world.push_back((g_ascii_isalnum(c) || c == '_') ? (char)c : '_');
    return world;
}

std::string handler_name_for(const std::string &id) {
    std::string name = "kiteExt_";
    for (unsigned char c : id)
        name.push_back((g_ascii_isalnum(c) || c == '_') ? (char)c : '_');
    return name;
}

bool resolve_extension_file(const std::string &base, const std::string &relative, std::string &result) {
    if (relative.empty() || g_path_is_absolute(relative.c_str()))
        return false;

    gchar *base_canonical = g_canonicalize_filename(base.c_str(), nullptr);
    gchar *joined = g_build_filename(base.c_str(), relative.c_str(), nullptr);
    gchar *canonical = g_canonicalize_filename(joined, nullptr);

    std::string prefix = base_canonical;
    if (!prefix.empty() && prefix.back() != G_DIR_SEPARATOR)
        prefix.push_back(G_DIR_SEPARATOR);

    bool inside = g_str_has_prefix(canonical, prefix.c_str());
    if (inside)
        result = canonical;

    g_free(base_canonical);
    g_free(joined);
    g_free(canonical);
    return inside;
}

bool read_text_file(const std::string &path, std::string &contents) {
    gchar *text = nullptr;
    gsize length = 0;
    GError *error = nullptr;
    if (!g_file_get_contents(path.c_str(), &text, &length, &error)) {
        g_clear_error(&error);
        return false;
    }
    contents.assign(text, length);
    g_free(text);
    return true;
}

std::string storage_file_path(const std::string &id) {
    gchar *name = g_strdup_printf("%s.json", id.c_str());
    gchar *path = g_build_filename(profile_extension_storage_dir(), name, nullptr);
    std::string result = path;
    g_free(path);
    g_free(name);
    return result;
}

// ---------------------------------------------------------------------------
// Manifest content script matching

void append_allowed_patterns(const std::vector<std::string> &matches, std::vector<std::string> &allow,
                             bool &allow_file) {
    for (const auto &pattern : matches) {
        if (pattern == "<all_urls>") {
            allow.push_back("http://*/*");
            allow.push_back("https://*/*");
        } else if (pattern.rfind("*://", 0) == 0) {
            std::string rest = pattern.substr(4);
            allow.push_back("http://" + rest);
            allow.push_back("https://" + rest);
        } else if (pattern.rfind("http://", 0) == 0 || pattern.rfind("https://", 0) == 0) {
            allow.push_back(pattern);
        } else if (pattern.rfind("file://", 0) == 0) {
            allow.push_back(pattern);
            allow_file = true;
        }
    }
}

std::vector<const gchar *> cstr_list(const std::vector<std::string> &values) {
    std::vector<const gchar *> list;
    list.reserve(values.size() + 1);
    for (const auto &value : values)
        list.push_back(value.c_str());
    list.push_back(nullptr);
    return list;
}

// ---------------------------------------------------------------------------
// chrome.* shim

std::string replace_token(std::string source, const std::string &token, const std::string &value) {
    size_t position = 0;
    while ((position = source.find(token, position)) != std::string::npos) {
        source.replace(position, token.size(), value);
        position += value.size();
    }
    return source;
}

const char *CHROME_SHIM = R"JS(
(function () {
  if (window.__kiteExtensionShim) return;
  window.__kiteExtensionShim = true;

  var EXT_ID = "__KITE_EXT_ID__";
  var HANDLER_NAME = "__KITE_HANDLER__";
  var CONTEXT = "__KITE_CONTEXT__";
  var MANIFEST = __KITE_MANIFEST__;
  var PERMISSIONS = __KITE_PERMISSIONS__;
  var runtimeListeners = [];
  var installedListeners = [];
  var startupListeners = [];

  function handler() {
    return window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers[HANDLER_NAME];
  }

  function post(payload) {
    var channel = handler();
    if (!channel) return Promise.reject(new Error("Kite extension API bridge is unavailable"));
    payload.extensionId = EXT_ID;
    payload.context = CONTEXT;
    return Promise.resolve(channel.postMessage(payload));
  }

  function call(type, data) {
    return post(Object.assign({type: type}, data || {})).then(function (reply) {
      if (typeof reply === "string") {
        try { reply = JSON.parse(reply); } catch (e) {}
      }
      if (!reply) throw new Error("The extension API returned no response.");
      if (reply.ok === false) throw new Error(reply.error || "Extension API request failed.");
      return reply.value;
    });
  }

  function withCallback(promise, callback) {
    if (typeof callback === "function") {
      promise.then(function (value) {
        chrome.runtime.lastError = undefined;
        callback(value);
      }, function (error) {
        chrome.runtime.lastError = {message: error && error.message ? error.message : String(error)};
        callback(undefined);
      });
      return undefined;
    }
    return promise;
  }

  function normalizeKeys(keys) {
    if (keys === null || keys === undefined) return null;
    if (typeof keys === "string") return [keys];
    if (Array.isArray(keys)) return keys;
    if (typeof keys === "object") return Object.keys(keys);
    return null;
  }

  function eventObject(listeners) {
    return {
      addListener: function (fn) {
        if (typeof fn === "function" && listeners.indexOf(fn) < 0) listeners.push(fn);
      },
      removeListener: function (fn) {
        var index = listeners.indexOf(fn);
        if (index >= 0) listeners.splice(index, 1);
      },
      hasListener: function (fn) { return listeners.indexOf(fn) >= 0; }
    };
  }

  var chrome = window.chrome || {};
  window.chrome = chrome;
  chrome.runtime = chrome.runtime || {};
  chrome.runtime.id = EXT_ID;
  chrome.runtime.lastError = undefined;
  chrome.runtime.getURL = function (path) {
    return "kite-ext://" + EXT_ID + "/" + String(path || "").replace(/^\/+/, "");
  };
  chrome.runtime.getManifest = function () { return MANIFEST; };
  chrome.runtime.sendMessage = function (message, callback) {
    if (typeof message === "string") {
      chrome.runtime.lastError = {message: "Could not establish connection. Receiving end does not exist."};
      if (typeof callback === "function") callback(undefined);
      return Promise.resolve(undefined);
    }
    var promise = call("runtime.sendMessage", {message: message}).catch(function (error) {
      chrome.runtime.lastError = {message: error && error.message ? error.message : "Could not establish connection."};
      return undefined;
    });
    return withCallback(promise, callback);
  };
  chrome.runtime.onMessage = eventObject(runtimeListeners);
  chrome.runtime.onInstalled = eventObject(installedListeners);
  chrome.runtime.onStartup = eventObject(startupListeners);

  function storageArea() {
    return {
      get: function (keys, callback) {
        return withCallback(call("storage.get", {keys: normalizeKeys(keys)}), callback);
      },
      set: function (items, callback) {
        return withCallback(call("storage.set", {items: items || {}}), callback);
      },
      remove: function (keys, callback) {
        return withCallback(call("storage.remove", {keys: normalizeKeys(keys)}), callback);
      },
      clear: function (callback) {
        return withCallback(call("storage.clear", {}), callback);
      }
    };
  }
  chrome.storage = chrome.storage || {};
  chrome.storage.local = storageArea();
  // Kite's folder sync handles cross-device data; extension sync storage uses
  // the same local store.
  chrome.storage.sync = storageArea();

  chrome.tabs = {
    query: function (info, callback) {
      return withCallback(call("tabs.query", {query: info || {}}), callback);
    },
    get: function (id, callback) {
      return withCallback(call("tabs.get", {id: id}), callback);
    },
    create: function (properties, callback) {
      return withCallback(call("tabs.create", {properties: properties || {}}), callback);
    },
    update: function (id, properties, callback) {
      return withCallback(call("tabs.update", {id: id, properties: properties || {}}), callback);
    },
    remove: function (ids, callback) {
      var list = Array.isArray(ids) ? ids : [ids];
      return withCallback(call("tabs.remove", {ids: list}), callback);
    },
    sendMessage: function (tabId, message, options, callback) {
      if (typeof options === "function") {
        callback = options;
        options = undefined;
      }
      var promise = call("tabs.sendMessage", {tabId: tabId, message: message}).catch(function (error) {
        chrome.runtime.lastError = {message: error && error.message ? error.message : "Could not establish connection."};
        return undefined;
      });
      return withCallback(promise, callback);
    }
  };

  chrome.notifications = {
    create: function (idOrOptions, maybeOptions, maybeCallback) {
      var id = "";
      var options = {};
      var callback = null;
      if (typeof idOrOptions === "string") {
        id = idOrOptions;
        options = maybeOptions || {};
        callback = typeof maybeCallback === "function" ? maybeCallback : null;
      } else {
        options = idOrOptions || {};
        callback = typeof maybeOptions === "function" ? maybeOptions : null;
      }
      return withCallback(call("notifications.create", {
        id: id,
        title: options.title || "",
        message: options.message || "",
        iconUrl: options.iconUrl || ""
      }), callback);
    }
  };

  function permissionSet() {
    var set = {};
    PERMISSIONS.forEach(function (permission) { set[permission] = true; });
    return set;
  }

  chrome.permissions = {
    getAll: function (callback) {
      var all = {permissions: PERMISSIONS.slice(), origins: []};
      if (typeof callback === "function") { callback(all); return undefined; }
      return Promise.resolve(all);
    },
    contains: function (request, callback) {
      var declared = permissionSet();
      var wanted = (request && request.permissions) || [];
      var ok = wanted.every(function (permission) { return !!declared[permission]; });
      if (typeof callback === "function") { callback(ok); return undefined; }
      return Promise.resolve(ok);
    },
    request: function (request, callback) {
      var declared = permissionSet();
      var wanted = (request && request.permissions) || [];
      var ok = wanted.every(function (permission) { return !!declared[permission]; });
      if (typeof callback === "function") { callback(ok); return undefined; }
      return Promise.resolve(ok);
    },
    remove: function (request, callback) {
      if (typeof callback === "function") { callback(false); return undefined; }
      return Promise.resolve(false);
    }
  };

  function respondNoReceiver(requestId) {
    var channel = handler();
    if (!channel) return;
    try {
      channel.postMessage({
        type: "runtime.response",
        extensionId: EXT_ID,
        requestId: requestId,
        response: JSON.stringify({__kiteNoReceiver: true})
      }).catch(function () {});
    } catch (e) {}
  }

  function dispatchRuntimeMessage(requestId, message, sender) {
    var responded = false;
    var waiting = 0;

    function sendResponse(value) {
      if (responded) return;
      responded = true;
      var text;
      try { text = JSON.stringify(value === undefined ? null : value); } catch (e) { text = "null"; }
      var channel = handler();
      if (!channel) return;
      try {
        channel.postMessage({
          type: "runtime.response",
          extensionId: EXT_ID,
          requestId: requestId,
          response: text
        }).catch(function () {});
      } catch (e) {}
    }

    var listeners = runtimeListeners.slice();
    if (!listeners.length) {
      respondNoReceiver(requestId);
      return;
    }

    for (var i = 0; i < listeners.length; i++) {
      if (responded) break;
      try {
        var result = listeners[i](message, sender || {}, sendResponse);
        if (result && typeof result.then === "function") {
          waiting++;
          result.then(sendResponse, function () {});
        } else if (result === true) {
          waiting++;
        }
      } catch (e) {}
    }
    if (!responded && waiting === 0) sendResponse(undefined);
  }

  function emitEvent(listeners, payload) {
    var copy = listeners.slice();
    for (var i = 0; i < copy.length; i++) {
      try { copy[i](payload); } catch (e) {}
    }
  }

  window.kiteExtDispatchMessage = function (requestId, message, sender) {
    dispatchRuntimeMessage(requestId, message, sender);
  };
  window.kiteExtEmitEvent = function (name, payload) {
    if (name === "runtime.onInstalled") emitEvent(installedListeners, payload);
    else if (name === "runtime.onStartup") emitEvent(startupListeners, payload);
  };
})();
)JS";

std::string build_shim(const ExtensionInfo &info, const char *context_name) {
    std::string permissions = "[";
    for (size_t i = 0; i < info.permissions.size(); i++) {
        if (i)
            permissions += ",";
        permissions += js_string(info.permissions[i]);
    }
    permissions += "]";

    std::string source = CHROME_SHIM;
    source = replace_token(source, "__KITE_EXT_ID__", info.id);
    source = replace_token(source, "__KITE_HANDLER__", handler_name_for(info.id));
    source = replace_token(source, "__KITE_CONTEXT__", context_name);
    source = replace_token(source, "__KITE_MANIFEST__", info.manifest_json.empty() ? "{}" : info.manifest_json);
    source = replace_token(source, "__KITE_PERMISSIONS__", permissions);
    return source;
}

// ---------------------------------------------------------------------------
// API helpers

const ExtensionInfo *find_extension(const std::string &id) {
    for (const auto &info : loaded_extensions) {
        if (info.id == id)
            return &info;
    }
    return nullptr;
}

bool has_permission(const ExtensionInfo &info, const char *permission) {
    return std::find(info.permissions.begin(), info.permissions.end(), permission) != info.permissions.end();
}

void reply_json(WebKitScriptMessageReply *reply, JSCContext *context, const std::string &json) {
    if (!reply || !context)
        return;
    JSCValue *value = jsc_value_new_string(context, json.c_str());
    webkit_script_message_reply_return_value(reply, value);
    g_object_unref(value);
}

gboolean respond_error(WebKitScriptMessageReply *reply, JSCContext *context, const std::string &message) {
    reply_json(reply, context, error_reply(message));
    return TRUE;
}

std::string new_request_id() {
    return "kite-" + std::to_string(++request_counter) + "-" + std::to_string(g_get_monotonic_time());
}

void pending_reply_free(gpointer data) {
    PendingReply *pending = (PendingReply *)data;
    if (!pending)
        return;
    if (pending->timeout_id) {
        g_source_remove(pending->timeout_id);
        pending->timeout_id = 0;
    }
    if (pending->reply)
        g_object_unref(pending->reply);
    if (pending->context)
        g_object_unref(pending->context);
    g_free(pending);
}

void complete_pending(const std::string &request_id, const std::string &payload) {
    if (!pending_replies)
        return;
    PendingReply *pending = (PendingReply *)g_hash_table_lookup(pending_replies, request_id.c_str());
    if (!pending)
        return;
    g_hash_table_steal(pending_replies, request_id.c_str());
    if (pending->timeout_id) {
        g_source_remove(pending->timeout_id);
        pending->timeout_id = 0;
    }
    reply_json(pending->reply, pending->context, payload);
    pending_reply_free(pending);
}

gboolean on_pending_timeout(gpointer user_data) {
    std::string request_id = (const gchar *)user_data;
    g_free(user_data);
    complete_pending(request_id,
                     error_reply("The extension did not respond in time. Check its background page."));
    return G_SOURCE_REMOVE;
}

PendingReply *track_pending(WebKitScriptMessageReply *reply, JSCContext *context, const std::string &extension_id,
                            const std::string &request_id) {
    if (!pending_replies)
        pending_replies = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, pending_reply_free);
    PendingReply *pending = g_new0(PendingReply, 1);
    pending->reply = (WebKitScriptMessageReply *)g_object_ref(reply);
    pending->context = (JSCContext *)g_object_ref(context);
    pending->extension_id = extension_id;
    pending->timeout_id = g_timeout_add_seconds(8, on_pending_timeout, g_strdup(request_id.c_str()));
    g_hash_table_insert(pending_replies, g_strdup(request_id.c_str()), pending);
    return pending;
}

std::string sender_json(const std::string &extension_id, const std::string &url) {
    return "{\"id\":" + js_string(extension_id) + ",\"url\":" + js_string(url) + "}";
}

std::string dispatch_script(const std::string &request_id, const std::string &message_json,
                            const std::string &sender) {
    return "if (window.kiteExtDispatchMessage) { try { window.kiteExtDispatchMessage(" + js_string(request_id) +
           "," + (message_json.empty() ? "null" : message_json) + "," + sender + "); } catch (e) {} }";
}

void evaluate_in_world(WebKitWebView *web_view, const std::string &script, const std::string &world) {
    if (!web_view || !webkit_web_view_get_context(web_view))
        return;
    webkit_web_view_evaluate_javascript(web_view, script.c_str(), (gssize)script.size(),
                                        world.empty() ? nullptr : world.c_str(), "kite-ext://api", nullptr, nullptr,
                                        nullptr);
}

BackgroundHost *find_background_host(const std::string &id) {
    for (auto *host : background_hosts) {
        if (host->extension_id == id)
            return host;
    }
    return nullptr;
}

void flush_background_queue(BackgroundHost *host) {
    if (!host)
        return;
    std::vector<DispatchRequest> queued;
    queued.swap(host->queued);
    for (const auto &request : queued)
        evaluate_in_world(host->web_view,
                          dispatch_script(request.request_id, request.message_json, request.sender_json), "");
}

bool dispatch_to_background(const ExtensionInfo &info, const std::string &request_id,
                            const std::string &message_json, const std::string &sender) {
    BackgroundHost *host = find_background_host(info.id);
    if (!host) {
        complete_pending(request_id, error_reply("Could not establish connection. The extension has no background page."));
        return false;
    }
    if (!host->loaded) {
        host->queued.push_back({request_id, message_json, sender});
        return true;
    }
    evaluate_in_world(host->web_view, dispatch_script(request_id, message_json, sender), "");
    return true;
}

bool dispatch_to_tab(const ExtensionInfo &info, int tab_id, const std::string &request_id,
                     const std::string &message_json, const std::string &sender) {
    WebKitWebView *web_view = tab_webview_for_id(tab_id);
    if (!web_view) {
        complete_pending(request_id, error_reply("No tab with the requested id exists."));
        return false;
    }
    if (info.script_count == 0) {
        complete_pending(request_id,
                         error_reply("Could not establish connection. The extension has no content scripts."));
        return false;
    }
    evaluate_in_world(web_view, dispatch_script(request_id, message_json, sender), world_name_for(info.id));
    return true;
}

// ---------------------------------------------------------------------------
// Storage

JSCValue *new_object(JSCContext *context) {
    return jsc_value_new_from_json(context, "{}");
}

JSCValue *load_storage(JSCContext *context, const std::string &id) {
    std::string path = storage_file_path(id);
    gchar *contents = nullptr;
    if (g_file_get_contents(path.c_str(), &contents, nullptr, nullptr)) {
        JSCValue *value = jsc_value_new_from_json(context, contents);
        g_free(contents);
        if (value && jsc_value_is_object(value))
            return value;
        if (value)
            g_object_unref(value);
    }
    return new_object(context);
}

bool save_storage(const std::string &id, JSCValue *value) {
    gchar *json = jsc_value_to_json(value, 0);
    if (!json)
        return false;
    std::string path = storage_file_path(id);
    bool ok = g_file_set_contents(path.c_str(), json, -1, nullptr);
    g_free(json);
    if (ok)
        g_chmod(path.c_str(), 0600);
    return ok;
}

void copy_keys(JSCValue *destination, JSCValue *source, const std::vector<std::string> &keys) {
    for (const auto &key : keys) {
        if (!jsc_value_object_has_property(source, key.c_str()))
            continue;
        JSCValue *value = jsc_value_object_get_property(source, key.c_str());
        if (value) {
            jsc_value_object_set_property(destination, key.c_str(), value);
            g_object_unref(value);
        }
    }
}

std::vector<std::string> requested_keys(JSCValue *keys) {
    std::vector<std::string> result;
    if (!keys || jsc_value_is_null(keys) || jsc_value_is_undefined(keys))
        return result;
    if (jsc_value_is_string(keys)) {
        gchar *text = jsc_value_to_string(keys);
        if (text && *text)
            result.push_back(text);
        g_free(text);
    } else if (jsc_value_is_array(keys)) {
        JSCValue *length_value = jsc_value_object_get_property(keys, "length");
        int length = length_value ? jsc_value_to_int32(length_value) : 0;
        if (length_value)
            g_object_unref(length_value);
        for (int i = 0; i < length; i++) {
            JSCValue *item = jsc_value_object_get_property_at_index(keys, (guint)i);
            if (item && jsc_value_is_string(item)) {
                gchar *text = jsc_value_to_string(item);
                if (text && *text)
                    result.push_back(text);
                g_free(text);
            }
            if (item)
                g_object_unref(item);
        }
    }
    return result;
}

std::string handle_storage(const ExtensionInfo &info, const std::string &type, JSCValue *payload,
                           JSCContext *context) {
    if (!has_permission(info, "storage"))
        return error_reply("The \"storage\" permission is required.");

    JSCValue *root = load_storage(context, info.id);
    JSCValue *keys = jsc_value_object_get_property(payload, "keys");
    std::string result_json = "null";

    if (type == "storage.get") {
        JSCValue *result = new_object(context);
        if (!keys || jsc_value_is_null(keys) || jsc_value_is_undefined(keys)) {
            gchar **names = jsc_value_object_enumerate_properties(root);
            if (names) {
                for (gchar **name = names; *name; name++)
                    copy_keys(result, root, {*name});
                g_strfreev(names);
            }
        } else {
            copy_keys(result, root, requested_keys(keys));
        }
        result_json = value_to_json(result);
        g_object_unref(result);
    } else if (type == "storage.set") {
        JSCValue *items = jsc_value_object_get_property(payload, "items");
        if (items && jsc_value_is_object(items)) {
            gchar **names = jsc_value_object_enumerate_properties(items);
            if (names) {
                for (gchar **name = names; *name; name++) {
                    JSCValue *value = jsc_value_object_get_property(items, *name);
                    if (value) {
                        jsc_value_object_set_property(root, *name, value);
                        g_object_unref(value);
                    }
                }
                g_strfreev(names);
            }
        }
        if (items)
            g_object_unref(items);
        if (!save_storage(info.id, root)) {
            g_object_unref(root);
            if (keys)
                g_object_unref(keys);
            return error_reply("Could not save extension storage.");
        }
        result_json = "null";
    } else if (type == "storage.remove") {
        for (const auto &key : requested_keys(keys))
            jsc_value_object_delete_property(root, key.c_str());
        save_storage(info.id, root);
        result_json = "null";
    } else if (type == "storage.clear") {
        JSCValue *empty = new_object(context);
        save_storage(info.id, empty);
        g_object_unref(empty);
        result_json = "null";
    } else {
        g_object_unref(root);
        if (keys)
            g_object_unref(keys);
        return error_reply("Unknown storage operation.");
    }

    if (keys)
        g_object_unref(keys);
    g_object_unref(root);
    return ok_reply(result_json);
}

// ---------------------------------------------------------------------------
// Tabs

std::string tab_to_json(const KiteTabInfo &tab) {
    std::string json = "{";
    json += "\"id\":" + std::to_string(tab.id);
    json += ",\"index\":" + std::to_string(tab.index);
    json += ",\"windowId\":1";
    json += ",\"active\":";
    json += tab.active ? "true" : "false";
    json += ",\"highlighted\":";
    json += tab.active ? "true" : "false";
    json += ",\"pinned\":false,\"incognito\":";
    json += tab.incognito ? "true" : "false";
    json += ",\"status\":\"complete\",\"url\":" + js_string(tab.url);
    json += ",\"title\":" + js_string(tab.title);
    json += "}";
    return json;
}

std::string tabs_array_json(const std::vector<KiteTabInfo> &tabs) {
    std::string json = "[";
    for (size_t i = 0; i < tabs.size(); i++) {
        if (i)
            json += ",";
        json += tab_to_json(tabs[i]);
    }
    json += "]";
    return json;
}

std::string handle_tabs(const ExtensionInfo &info, const std::string &type, JSCValue *payload,
                        JSCContext *context) {
    if (!has_permission(info, "tabs"))
        return error_reply("The \"tabs\" permission is required.");

    if (type == "tabs.query") {
        JSCValue *query = jsc_value_object_get_property(payload, "query");
        bool active_only = query && json_bool(query, "active");
        bool include_incognito = query && json_bool(query, "incognito");
        if (query)
            g_object_unref(query);

        std::vector<KiteTabInfo> filtered;
        for (const auto &tab : tabs_list()) {
            if (active_only && !tab.active)
                continue;
            if (!include_incognito && tab.incognito)
                continue;
            filtered.push_back(tab);
        }
        return ok_reply(tabs_array_json(filtered));
    }

    if (type == "tabs.get") {
        int id = json_int(payload, "id");
        for (const auto &tab : tabs_list()) {
            if (tab.id == id)
                return ok_reply(tab_to_json(tab));
        }
        return error_reply("No tab with id " + std::to_string(id) + " exists.");
    }

    if (type == "tabs.create") {
        JSCValue *properties = jsc_value_object_get_property(payload, "properties");
        std::string url = properties ? json_string(properties, "url") : "";
        bool active = properties ? json_bool(properties, "active") : true;
        if (properties)
            g_object_unref(properties);
        if (url.empty())
            url = "about:blank";

        int previous_active_id = 0;
        for (const auto &tab : tabs_list()) {
            if (tab.active) {
                previous_active_id = tab.id;
                break;
            }
        }

        create_new_tab(url.c_str());
        WebKitWebView *view = get_active_webview();
        for (const auto &tab : tabs_list()) {
            if (view && tab.id == tab_id_for_webview(view)) {
                if (!active && previous_active_id > 0)
                    tab_activate_id(previous_active_id);
                return ok_reply(tab_to_json(tab));
            }
        }
        return ok_reply("null");
    }

    if (type == "tabs.update") {
        int id = json_int(payload, "id");
        JSCValue *properties = jsc_value_object_get_property(payload, "properties");
        std::string url = properties ? json_string(properties, "url") : "";
        bool active = properties ? json_bool(properties, "active") : false;
        if (properties)
            g_object_unref(properties);
        WebKitWebView *view = tab_webview_for_id(id);
        if (!view)
            return error_reply("No tab with id " + std::to_string(id) + " exists.");
        if (!url.empty())
            webkit_web_view_load_uri(view, url.c_str());
        if (active)
            tab_activate_id(id);
        for (const auto &tab : tabs_list()) {
            if (tab.id == id)
                return ok_reply(tab_to_json(tab));
        }
        return ok_reply("null");
    }

    if (type == "tabs.remove") {
        JSCValue *ids = jsc_value_object_get_property(payload, "ids");
        std::vector<int> values;
        if (ids && jsc_value_is_array(ids)) {
            JSCValue *length_value = jsc_value_object_get_property(ids, "length");
            int length = length_value ? jsc_value_to_int32(length_value) : 0;
            if (length_value)
                g_object_unref(length_value);
            for (int i = 0; i < length; i++) {
                JSCValue *item = jsc_value_object_get_property_at_index(ids, (guint)i);
                if (item && jsc_value_is_number(item))
                    values.push_back(jsc_value_to_int32(item));
                if (item)
                    g_object_unref(item);
            }
        } else if (ids && jsc_value_is_number(ids)) {
            values.push_back(jsc_value_to_int32(ids));
        }
        if (ids)
            g_object_unref(ids);
        for (int id : values)
            tab_close_id(id);
        return ok_reply("null");
    }

    return error_reply("Unknown tabs operation.");
}

// ---------------------------------------------------------------------------
// Notifications

std::string handle_notifications(const ExtensionInfo &info, const std::string &type, JSCValue *payload) {
    if (type != "notifications.create")
        return error_reply("Unknown notifications operation.");
    if (!has_permission(info, "notifications"))
        return error_reply("The \"notifications\" permission is required.");

    std::string id = json_string(payload, "id");
    if (id.empty())
        id = "kite-" + std::to_string(++request_counter);
    std::string title = json_string(payload, "title");
    std::string message = json_string(payload, "message");
    if (title.empty())
        title = info.name;
    if (title.empty())
        title = "Kite extension";
    if (message.size() > 2000)
        message.resize(2000);

    gchar *program = g_find_program_in_path("notify-send");
    if (program) {
        gchar *argv[] = {program, (gchar *)"--app-name=Kite", (gchar *)"--", (gchar *)title.c_str(),
                         (gchar *)message.c_str(), nullptr};
        GError *error = nullptr;
        g_spawn_async(nullptr, argv, nullptr, (GSpawnFlags)(G_SPAWN_SEARCH_PATH | G_SPAWN_DEFAULT), nullptr, nullptr,
                      nullptr, &error);
        if (error) {
            g_printerr("[Kite] Could not show an extension notification: %s\n", error->message);
            g_error_free(error);
        }
        g_free(program);
    } else {
        g_print("[Kite] Extension notification (%s): %s — %s\n", info.id.c_str(), title.c_str(), message.c_str());
    }

    return ok_reply(js_string(id));
}

// ---------------------------------------------------------------------------
// Script message routing

gboolean handle_api_message(JSCValue *value, WebKitScriptMessageReply *reply) {
    JSCContext *context = jsc_value_get_context(value);
    std::string type = json_string(value, "type");
    std::string extension_id = json_string(value, "extensionId");
    std::string message_context = json_string(value, "context");

    if (type.empty() || extension_id.empty())
        return respond_error(reply, context, "Malformed extension API message.");

    if (type == "runtime.response") {
        std::string request_id = json_string(value, "requestId");
        std::string response = json_string(value, "response");
        std::string payload;
        if (response.find("__kiteNoReceiver") != std::string::npos)
            payload = error_reply("Could not establish connection. Receiving end does not exist.");
        else
            payload = ok_reply(response.empty() ? "null" : response);
        complete_pending(request_id, payload);
        reply_json(reply, context, "{\"ok\":true,\"value\":null}");
        return TRUE;
    }

    const ExtensionInfo *info = find_extension(extension_id);
    if (!info || !info->enabled)
        return respond_error(reply, context, "That extension is not enabled.");

    if (type == "storage.get" || type == "storage.set" || type == "storage.remove" || type == "storage.clear") {
        std::string result = handle_storage(*info, type, value, context);
        reply_json(reply, context, result);
        return TRUE;
    }

    if (type.rfind("tabs.", 0) == 0 && type != "tabs.sendMessage") {
        std::string result = handle_tabs(*info, type, value, context);
        reply_json(reply, context, result);
        return TRUE;
    }

    if (type.rfind("notifications.", 0) == 0) {
        std::string result = handle_notifications(*info, type, value);
        reply_json(reply, context, result);
        return TRUE;
    }

    if (type == "runtime.sendMessage") {
        if (message_context == "background") {
            reply_json(reply, context,
                       error_reply("Could not establish connection. Receiving end does not exist."));
            return TRUE;
        }
        JSCValue *message = jsc_value_object_get_property(value, "message");
        std::string message_json = value_to_json(message);
        if (message)
            g_object_unref(message);
        std::string url = json_string(value, "url");
        std::string request_id = new_request_id();
        track_pending(reply, context, info->id, request_id);
        dispatch_to_background(*info, request_id, message_json, sender_json(info->id, url));
        return TRUE;
    }

    if (type == "tabs.sendMessage") {
        int tab_id = json_int(value, "tabId");
        JSCValue *message = jsc_value_object_get_property(value, "message");
        std::string message_json = value_to_json(message);
        if (message)
            g_object_unref(message);
        std::string url = json_string(value, "url");
        std::string request_id = new_request_id();
        track_pending(reply, context, info->id, request_id);
        dispatch_to_tab(*info, tab_id, request_id, message_json, sender_json(info->id, url));
        return TRUE;
    }

    return respond_error(reply, context, "Unknown extension API operation: " + type);
}

void on_script_message(WebKitUserContentManager *manager, JSCValue *value, WebKitScriptMessageReply *reply,
                       gpointer user_data) {
    (void)manager;
    (void)user_data;
    if (!value || !reply)
        return;
    JSCContext *context = jsc_value_get_context(value);
    if (!handle_api_message(value, reply))
        reply_json(reply, context, "{\"ok\":true,\"value\":null}");
}

// ---------------------------------------------------------------------------
// Extension pages and background hosts

std::string mime_type_for(const std::string &path) {
    if (g_str_has_suffix(path.c_str(), ".html") || g_str_has_suffix(path.c_str(), ".htm"))
        return "text/html; charset=utf-8";
    if (g_str_has_suffix(path.c_str(), ".js") || g_str_has_suffix(path.c_str(), ".mjs"))
        return "application/javascript; charset=utf-8";
    if (g_str_has_suffix(path.c_str(), ".css"))
        return "text/css; charset=utf-8";
    if (g_str_has_suffix(path.c_str(), ".json"))
        return "application/json; charset=utf-8";
    if (g_str_has_suffix(path.c_str(), ".svg"))
        return "image/svg+xml";
    if (g_str_has_suffix(path.c_str(), ".png"))
        return "image/png";
    if (g_str_has_suffix(path.c_str(), ".jpg") || g_str_has_suffix(path.c_str(), ".jpeg"))
        return "image/jpeg";
    if (g_str_has_suffix(path.c_str(), ".gif"))
        return "image/gif";
    if (g_str_has_suffix(path.c_str(), ".webp"))
        return "image/webp";
    if (g_str_has_suffix(path.c_str(), ".woff"))
        return "font/woff";
    if (g_str_has_suffix(path.c_str(), ".woff2"))
        return "font/woff2";
    if (g_str_has_suffix(path.c_str(), ".ttf"))
        return "font/ttf";
    if (g_str_has_suffix(path.c_str(), ".txt"))
        return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

std::string generated_background_page(const ExtensionInfo &info) {
    std::string html = "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>" +
                       std::string(g_markup_escape_text(info.name.c_str(), -1)) +
                       "</title></head><body>";
    for (const auto &script : info.background_scripts) {
        std::string relative = script;
        while (!relative.empty() && relative.front() == '/')
            relative.erase(relative.begin());
        gchar *escaped = g_markup_escape_text(relative.c_str(), -1);
        html += "<script src=\"" + std::string(escaped ? escaped : "") + "\"></script>";
        g_free(escaped);
    }
    html += "</body></html>";
    return html;
}

void finish_request(WebKitURISchemeRequest *request, const std::string &contents, const std::string &mime) {
    GBytes *bytes = g_bytes_new(contents.data(), contents.size());
    GInputStream *stream = g_memory_input_stream_new_from_bytes(bytes);
    webkit_uri_scheme_request_finish(request, stream, (gint64)g_bytes_get_size(bytes), mime.c_str());
    g_object_unref(stream);
    g_bytes_unref(bytes);
}

void finish_request_error(WebKitURISchemeRequest *request, const char *message) {
    GError *error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, message);
    webkit_uri_scheme_request_finish_error(request, error);
    g_error_free(error);
}

void on_extension_request(WebKitURISchemeRequest *request, gpointer user_data) {
    const gchar *uri = webkit_uri_scheme_request_get_uri(request);
    GError *error = nullptr;
    GUri *parsed = uri ? g_uri_parse(uri, G_URI_FLAGS_NONE, &error) : nullptr;
    if (!parsed) {
        g_clear_error(&error);
        finish_request_error(request, "Invalid extension URL.");
        return;
    }

    const gchar *host = g_uri_get_host(parsed);
    const gchar *path = g_uri_get_path(parsed);
    std::string id = host ? host : "";
    std::string relative = path ? path : "";
    while (!relative.empty() && relative.front() == '/')
        relative.erase(relative.begin());
    g_uri_unref(parsed);

    const ExtensionInfo *info = find_extension(id);
    if (!id.empty() && valid_identifier(id)) {
        for (const auto &candidate : loaded_extensions) {
            if (candidate.id == id)
                info = &candidate;
        }
    }
    if (!info || !info->enabled) {
        finish_request_error(request, "Extension not found or disabled.");
        return;
    }

    if (relative.empty() || relative == "__generated_background_page.html" ||
        relative == "_generated_background_page.html") {
        finish_request(request, generated_background_page(*info), "text/html; charset=utf-8");
        return;
    }

    std::string file_path;
    if (!resolve_extension_file(info->path, relative, file_path)) {
        finish_request_error(request, "That extension file does not exist.");
        return;
    }

    gchar *contents = nullptr;
    gsize length = 0;
    if (!g_file_get_contents(file_path.c_str(), &contents, &length, nullptr)) {
        finish_request_error(request, "Could not read the extension file.");
        return;
    }
    std::string body(contents, length);
    g_free(contents);
    finish_request(request, body, mime_type_for(relative));
}

std::string background_url(const ExtensionInfo &info) {
    std::string relative = info.background_page;
    while (!relative.empty() && relative.front() == '/')
        relative.erase(relative.begin());
    if (relative.empty())
        relative = "__generated_background_page.html";
    return "kite-ext://" + info.id + "/" + relative;
}

std::string install_reason(ExtensionInfo &info) {
    gchar *marker_path = g_build_filename(info.path.c_str(), ".kite-installed-version", nullptr);
    gchar *old_version = nullptr;
    g_file_get_contents(marker_path, &old_version, nullptr, nullptr);
    gchar *stripped = old_version ? g_strstrip(old_version) : nullptr;
    std::string previous = stripped ? stripped : "";
    std::string current = info.version.empty() ? "unknown" : info.version;
    std::string reason;
    if (previous.empty())
        reason = "install";
    else if (previous != current)
        reason = "update";
    if (reason != "")
        g_file_set_contents(marker_path, current.c_str(), -1, nullptr);
    g_free(stripped);
    g_free(old_version);
    g_free(marker_path);
    return reason;
}

void on_background_load_changed(WebKitWebView *web_view, WebKitLoadEvent event, gpointer user_data) {
    BackgroundHost *host = (BackgroundHost *)user_data;
    if (!host || event != WEBKIT_LOAD_FINISHED)
        return;

    host->loaded = true;
    flush_background_queue(host);

    if (host->announced)
        return;
    host->announced = true;

    std::string script;
    if (!host->install_reason.empty()) {
        script += "if (window.kiteExtEmitEvent) window.kiteExtEmitEvent('runtime.onInstalled',{reason:" +
                  js_string(host->install_reason) + "});";
    }
    script += "if (window.kiteExtEmitEvent) window.kiteExtEmitEvent('runtime.onStartup',{});";
    evaluate_in_world(web_view, script, "");
}

void destroy_background_host(BackgroundHost *host) {
    if (!host)
        return;
    if (host->window) {
        gtk_widget_destroy(host->window);
        host->window = nullptr;
        host->web_view = nullptr;
    }
    if (host->manager) {
        g_object_unref(host->manager);
        host->manager = nullptr;
    }
    delete host;
}

void clear_background_hosts() {
    for (auto *host : background_hosts)
        destroy_background_host(host);
    background_hosts.clear();
}

void create_background_host(ExtensionInfo &info) {
    if (!info.enabled || !info.has_background())
        return;
    if (!web_context) {
        g_printerr("[Kite] Cannot start the %s background page without a WebKit context.\n", info.id.c_str());
        return;
    }

    BackgroundHost *host = new BackgroundHost();
    host->extension_id = info.id;
    host->handler_name = handler_name_for(info.id);
    host->install_reason = install_reason(info);
    host->manager = webkit_user_content_manager_new();

    std::string shim = build_shim(info, "background");
    std::vector<std::string> allow = {"kite-ext://" + info.id + "/*"};
    std::vector<std::string> block = {"http://*/*", "https://*/*", "file:///*", "kite://*"};
    auto allow_list = cstr_list(allow);
    auto block_list = cstr_list(block);
    WebKitUserScript *shim_script = webkit_user_script_new(
        shim.c_str(), WEBKIT_USER_CONTENT_INJECT_TOP_FRAME, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
        allow_list.data(), block_list.data());
    if (shim_script) {
        webkit_user_content_manager_add_script(host->manager, shim_script);
        webkit_user_script_unref(shim_script);
    }

    std::string signal_name = "script-message-with-reply-received::" + host->handler_name;
    g_signal_connect(host->manager, signal_name.c_str(), G_CALLBACK(on_script_message), nullptr);
    if (!webkit_user_content_manager_register_script_message_handler_with_reply(host->manager,
                                                                                host->handler_name.c_str(), nullptr)) {
        g_printerr("[Kite] Could not register the %s background API bridge.\n", info.id.c_str());
    }

    host->web_view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "web-context", web_context,
                                                  "user-content-manager", host->manager, nullptr));
    g_object_set_data(G_OBJECT(host->web_view), "is_incognito", GINT_TO_POINTER(0));
    g_signal_connect(host->web_view, "load-changed", G_CALLBACK(on_background_load_changed), host);

    host->window = gtk_offscreen_window_new();
    gtk_container_add(GTK_CONTAINER(host->window), GTK_WIDGET(host->web_view));
    gtk_widget_show_all(host->window);
    gtk_widget_hide(host->window);

    background_hosts.push_back(host);
    std::string url = background_url(info);
    g_printerr("[Kite DEBUG] background extension=%s url=%s\n", info.id.c_str(), url.c_str());
    webkit_web_view_load_uri(host->web_view, url.c_str());
}

// ---------------------------------------------------------------------------
// Content script injection

void unregister_content_handlers() {
    WebKitUserContentManager *manager = adblock_content_manager();
    for (const auto &handler : content_handlers) {
        if (manager) {
            if (!handler.world.empty())
                webkit_user_content_manager_unregister_script_message_handler_in_world(
                    manager, handler.name.c_str(), handler.world.c_str());
            else
                webkit_user_content_manager_unregister_script_message_handler(manager, handler.name.c_str());
            if (handler.signal_id)
                g_signal_handler_disconnect(manager, handler.signal_id);
        }
    }
    content_handlers.clear();
}

void register_content_handler(const ExtensionInfo &info) {
    WebKitUserContentManager *manager = adblock_content_manager();
    if (!manager)
        return;

    RegisteredHandler handler;
    handler.name = handler_name_for(info.id);
    handler.world = world_name_for(info.id);
    std::string signal_name = "script-message-with-reply-received::" + handler.name;
    handler.signal_id = g_signal_connect(manager, signal_name.c_str(), G_CALLBACK(on_script_message), nullptr);
    if (!webkit_user_content_manager_register_script_message_handler_with_reply(
            manager, handler.name.c_str(), handler.world.c_str())) {
        if (handler.signal_id)
            g_signal_handler_disconnect(manager, handler.signal_id);
        handler.signal_id = 0;
        g_printerr("[Kite] Could not register the %s content API bridge.\n", info.id.c_str());
        return;
    }
    content_handlers.push_back(handler);
}

void inject_content_scripts(ExtensionInfo &info, const std::vector<ContentScriptSpec> &scripts) {
    WebKitUserContentManager *manager = adblock_content_manager();
    if (!manager) {
        info.error = "The WebKit user content manager is unavailable.";
        return;
    }

    std::string world = world_name_for(info.id);
    bool has_javascript = false;
    std::vector<std::string> union_allow;
    bool union_allow_file = false;
    for (const auto &spec : scripts) {
        if (spec.js.empty())
            continue;
        has_javascript = true;
        append_allowed_patterns(spec.matches, union_allow, union_allow_file);
    }
    std::sort(union_allow.begin(), union_allow.end());
    union_allow.erase(std::unique(union_allow.begin(), union_allow.end()), union_allow.end());

    if (has_javascript && !union_allow.empty()) {
        std::vector<std::string> block = {"kite://*", "kite://*/*"};
        if (!union_allow_file)
            block.push_back("file:///*");
        auto allow_list = cstr_list(union_allow);
        auto block_list = cstr_list(block);

        std::string shim = build_shim(info, "content");
        WebKitUserScript *shim_script = webkit_user_script_new_for_world(
            shim.c_str(), WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
            world.c_str(), allow_list.data(), block_list.data());
        if (shim_script) {
            webkit_user_content_manager_add_script(manager, shim_script);
            injected_scripts.push_back(shim_script);
        }
        register_content_handler(info);
    }

    for (const auto &spec : scripts) {
        std::vector<std::string> allow;
        bool allow_file = false;
        append_allowed_patterns(spec.matches, allow, allow_file);
        if (allow.empty()) {
            if (info.error.empty())
                info.error = "A content script has no supported match patterns.";
            continue;
        }

        std::vector<std::string> block = {"kite://*", "kite://*/*"};
        if (!allow_file)
            block.push_back("file:///*");

        auto allow_list = cstr_list(allow);
        auto block_list = cstr_list(block);
        WebKitUserContentInjectedFrames frames =
            spec.all_frames ? WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES : WEBKIT_USER_CONTENT_INJECT_TOP_FRAME;
        WebKitUserScriptInjectionTime injection_time =
            spec.run_at == "document_start" ? WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START
                                            : WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END;

        for (const auto &relative : spec.js) {
            std::string path;
            std::string source;
            if (!resolve_extension_file(info.path, relative, path) || !read_text_file(path, source)) {
                if (info.error.empty())
                    info.error = "Could not read script " + relative;
                continue;
            }
            WebKitUserScript *script = webkit_user_script_new_for_world(
                source.c_str(), frames, injection_time, world.c_str(), allow_list.data(), block_list.data());
            if (!script) {
                if (info.error.empty())
                    info.error = "WebKit rejected script " + relative;
                continue;
            }
            webkit_user_content_manager_add_script(manager, script);
            injected_scripts.push_back(script);
        }

        for (const auto &relative : spec.css) {
            std::string path;
            std::string source;
            if (!resolve_extension_file(info.path, relative, path) || !read_text_file(path, source)) {
                if (info.error.empty())
                    info.error = "Could not read style " + relative;
                continue;
            }
            WebKitUserStyleSheet *sheet = webkit_user_style_sheet_new(
                source.c_str(), frames, WEBKIT_USER_STYLE_LEVEL_USER, allow_list.data(), block_list.data());
            if (!sheet) {
                if (info.error.empty())
                    info.error = "WebKit rejected style " + relative;
                continue;
            }
            webkit_user_content_manager_add_style_sheet(manager, sheet);
            injected_styles.push_back(sheet);
        }
    }
}

void clear_injections() {
    WebKitUserContentManager *manager = adblock_content_manager();
    if (manager) {
        for (auto *script : injected_scripts)
            webkit_user_content_manager_remove_script(manager, script);
        for (auto *sheet : injected_styles)
            webkit_user_content_manager_remove_style_sheet(manager, sheet);
    }
    for (auto *script : injected_scripts)
        webkit_user_script_unref(script);
    for (auto *sheet : injected_styles)
        webkit_user_style_sheet_unref(sheet);
    injected_scripts.clear();
    injected_styles.clear();
    unregister_content_handlers();
}

// ---------------------------------------------------------------------------
// Folder install helpers

bool copy_directory(const gchar *source, const gchar *destination, std::string &error) {
    if (g_mkdir_with_parents(destination, 0755) != 0) {
        error = g_strerror(errno);
        return false;
    }

    GError *dir_error = nullptr;
    GDir *dir = g_dir_open(source, 0, &dir_error);
    if (!dir) {
        error = dir_error ? dir_error->message : "Could not read the extension folder.";
        g_clear_error(&dir_error);
        return false;
    }

    bool ok = true;
    const gchar *name = nullptr;
    while (ok && (name = g_dir_read_name(dir))) {
        gchar *child_source = g_build_filename(source, name, nullptr);
        gchar *child_destination = g_build_filename(destination, name, nullptr);

        if (g_file_test(child_source, G_FILE_TEST_IS_DIR) &&
            !g_file_test(child_source, G_FILE_TEST_IS_SYMLINK)) {
            ok = copy_directory(child_source, child_destination, error);
        } else if (g_file_test(child_source, G_FILE_TEST_IS_REGULAR)) {
            GFile *from = g_file_new_for_path(child_source);
            GFile *to = g_file_new_for_path(child_destination);
            GError *copy_error = nullptr;
            if (!g_file_copy(from, to, G_FILE_COPY_OVERWRITE, nullptr, nullptr, nullptr, &copy_error)) {
                error = copy_error ? copy_error->message : "Could not copy an extension file.";
                g_clear_error(&copy_error);
                ok = false;
            }
            g_object_unref(from);
            g_object_unref(to);
        }

        g_free(child_source);
        g_free(child_destination);
    }
    g_dir_close(dir);
    return ok;
}

bool remove_directory(const gchar *path, std::string &error) {
    GDir *dir = g_dir_open(path, 0, nullptr);
    if (!dir) {
        error = g_strerror(errno);
        return false;
    }

    bool ok = true;
    const gchar *name = nullptr;
    while (ok && (name = g_dir_read_name(dir))) {
        gchar *child = g_build_filename(path, name, nullptr);
        if (g_file_test(child, G_FILE_TEST_IS_DIR) && !g_file_test(child, G_FILE_TEST_IS_SYMLINK)) {
            ok = remove_directory(child, error);
        } else if (g_remove(child) != 0) {
            error = g_strerror(errno);
            ok = false;
        }
        g_free(child);
    }
    g_dir_close(dir);

    if (ok && g_rmdir(path) != 0) {
        error = g_strerror(errno);
        ok = false;
    }
    return ok;
}

} // namespace

const gchar *extensions_directory() {
    if (!extension_root)
        extension_root = g_build_filename(profile_data_dir(), "extensions", nullptr);
    return extension_root;
}

bool extension_has_permission(const gchar *id, const gchar *permission) {
    if (!id || !permission)
        return false;
    const ExtensionInfo *info = find_extension(id);
    return info && has_permission(*info, permission);
}

void extensions_register_scheme(WebKitWebContext *context) {
    if (!context)
        return;
    if (g_object_get_data(G_OBJECT(context), "kite-ext-scheme-registered"))
        return;
    webkit_web_context_register_uri_scheme(context, "kite-ext", on_extension_request, nullptr, nullptr);
    g_object_set_data(G_OBJECT(context), "kite-ext-scheme-registered", GINT_TO_POINTER(1));
}

void extensions_reload() {
    if (!pending_replies)
        pending_replies = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, pending_reply_free);

    clear_background_hosts();
    clear_injections();
    loaded_extensions.clear();

    const gchar *root = extensions_directory();
    g_mkdir_with_parents(root, 0700);

    GDir *dir = g_dir_open(root, 0, nullptr);
    if (!dir)
        return;

    const gchar *name = nullptr;
    while ((name = g_dir_read_name(dir))) {
        if (name[0] == '.')
            continue;

        gchar *path = g_build_filename(root, name, nullptr);
        if (!g_file_test(path, G_FILE_TEST_IS_DIR)) {
            g_free(path);
            continue;
        }

        gchar *manifest_path = g_build_filename(path, "manifest.json", nullptr);
        if (!g_file_test(manifest_path, G_FILE_TEST_IS_REGULAR)) {
            g_free(manifest_path);
            g_free(path);
            continue;
        }

        ExtensionInfo info;
        info.id = name;
        info.path = path;

        std::vector<ContentScriptSpec> scripts;
        info.valid = parse_manifest(manifest_path, info, scripts);

        gchar *disabled_path = g_build_filename(path, "disabled", nullptr);
        bool disabled = g_file_test(disabled_path, G_FILE_TEST_EXISTS);
        g_free(disabled_path);
        info.enabled = info.valid && !disabled;

        if (info.name.empty())
            info.name = name;
        if (info.enabled)
            inject_content_scripts(info, scripts);

        loaded_extensions.push_back(info);
        g_free(manifest_path);
        g_free(path);
    }
    g_dir_close(dir);

    std::sort(loaded_extensions.begin(), loaded_extensions.end(), [](const ExtensionInfo &a, const ExtensionInfo &b) {
        return g_ascii_strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });

    for (auto &info : loaded_extensions) {
        if (info.enabled && info.has_background())
            create_background_host(info);
    }

    g_print("[Kite] Loaded %zu extension(s).\n", loaded_extensions.size());
}

void extensions_init(WebKitWebContext *context) {
    if (context)
        web_context = context;
    if (!web_context)
        web_context = app_webkit_context();
    if (!pending_replies)
        pending_replies = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, pending_reply_free);

    g_mkdir_with_parents(extensions_directory(), 0700);
    g_mkdir_with_parents(profile_extension_storage_dir(), 0700);
    extensions_register_scheme(web_context);
    extensions_reload();
}

std::vector<ExtensionInfo> extensions_list() {
    return loaded_extensions;
}

bool extensions_install_from_folder(const gchar *source_dir, std::string *error) {
    std::string ignored;
    std::string &message = error ? *error : ignored;
    message.clear();

    if (!source_dir || !*source_dir) {
        message = "Choose an extension folder first.";
        return false;
    }

    gchar *source = g_canonicalize_filename(source_dir, nullptr);
    gchar *manifest_path = g_build_filename(source, "manifest.json", nullptr);
    if (!g_file_test(manifest_path, G_FILE_TEST_IS_REGULAR)) {
        message = "The selected folder does not contain a manifest.json file.";
        g_free(manifest_path);
        g_free(source);
        return false;
    }

    ExtensionInfo probe;
    std::vector<ContentScriptSpec> scripts;
    if (!parse_manifest(manifest_path, probe, scripts)) {
        message = probe.error.empty() ? "The extension manifest is invalid." : probe.error;
        g_free(manifest_path);
        g_free(source);
        return false;
    }

    gchar *basename = g_path_get_basename(source);
    std::string id = sanitize_identifier(basename);
    g_free(basename);
    if (id.empty())
        id = "extension";

    gchar *destination = g_build_filename(extensions_directory(), id.c_str(), nullptr);
    if (g_file_test(destination, G_FILE_TEST_EXISTS)) {
        message = "An extension folder named \"" + id + "\" is already installed. Remove it first to reinstall.";
        g_free(destination);
        g_free(manifest_path);
        g_free(source);
        return false;
    }

    bool copied = copy_directory(source, destination, message);
    g_free(manifest_path);
    g_free(source);
    if (!copied) {
        std::string cleanup_error;
        remove_directory(destination, cleanup_error);
        g_free(destination);
        return false;
    }

    gchar *disabled_path = g_build_filename(destination, "disabled", nullptr);
    g_remove(disabled_path);
    g_free(disabled_path);
    g_free(destination);

    extensions_reload();
    return true;
}

bool extensions_set_enabled(const gchar *id, bool enabled) {
    if (!id || !valid_identifier(id))
        return false;

    gchar *path = extension_path(id);
    if (!g_file_test(path, G_FILE_TEST_IS_DIR)) {
        g_free(path);
        return false;
    }

    gchar *disabled_path = g_build_filename(path, "disabled", nullptr);
    bool ok = enabled ? g_remove(disabled_path) == 0 || !g_file_test(disabled_path, G_FILE_TEST_EXISTS)
                      : g_file_set_contents(disabled_path, "Disabled from kite://extensions\n", -1, nullptr);
    g_free(disabled_path);
    g_free(path);

    if (ok)
        extensions_reload();
    return ok;
}

bool extensions_remove(const gchar *id) {
    if (!id || !valid_identifier(id))
        return false;

    gchar *path = extension_path(id);
    if (!g_file_test(path, G_FILE_TEST_IS_DIR)) {
        g_free(path);
        return false;
    }

    std::string error;
    bool ok = remove_directory(path, error);
    g_free(path);
    if (ok) {
        g_remove(storage_file_path(id).c_str());
        extensions_reload();
    }
    return ok;
}
