#include "dark_mode.h"

#include "adblock.h"
#include "browser_window.h"
#include "settings.h"

// Dark mode is applied in two layers:
//
//   1. WebKit is told to prefer a dark color scheme (through the GTK theme and
//      through a small matchMedia patch), so sites with a native dark theme
//      use it instead of being recolored by us.
//   2. For light-only sites, a small color transformer walks the DOM and maps
//      light backgrounds, dark text and light borders to dark equivalents. It
//      keeps hues instead of blindly inverting the page, so photos, logos and
//      brand colors stay recognizable.
//
// The same script is injected into every frame at document-start and can also
// be evaluated on already-loaded pages when the toolbar toggle is used.

static const char *DARK_MODE_JS = R"JS(
(function () {
  'use strict';

  if (location.protocol === 'kite:' || location.protocol === 'about:') return;
  if (document.contentType === 'application/pdf') return;

  if (window.__kiteDarkMode) {
    window.__kiteDarkMode.enable();
    return;
  }

  var STYLE_ID = 'kite-dark-mode-style';
  var originalMatchMedia = window.matchMedia ? window.matchMedia.bind(window) : null;
  var originalInline = new WeakMap();
  var trackedElements = new Set();
  var observer = null;
  var enabled = false;
  var queue = [];
  var scheduled = false;

  function mediaList(query, matches) {
    return {
      matches: matches,
      media: query,
      onchange: null,
      addListener: function () {},
      removeListener: function () {},
      addEventListener: function () {},
      removeEventListener: function () {},
      dispatchEvent: function () { return false; }
    };
  }

  function patchedMatchMedia(query) {
    if (!enabled || !originalMatchMedia)
      return originalMatchMedia ? originalMatchMedia(query) : mediaList(query, false);
    var text = String(query || '');
    if (/prefers-color-scheme\s*:\s*dark/i.test(text)) return mediaList(query, true);
    if (/prefers-color-scheme\s*:\s*light/i.test(text)) return mediaList(query, false);
    return originalMatchMedia(query);
  }

  function parseColor(value) {
    if (!value || value === 'transparent' || value === 'none') return null;
    var match = value.match(/^rgba?\(\s*([0-9.]+)[,\s]+([0-9.]+)[,\s]+([0-9.]+)(?:\s*[,/]\s*([0-9.]+%?))?\s*\)$/i);
    if (!match) return null;
    var alpha = match[4] === undefined ? 1 : parseFloat(match[4]);
    if (String(match[4]).slice(-1) === '%') alpha /= 100;
    return {
      r: Math.max(0, Math.min(255, parseInt(match[1], 10))),
      g: Math.max(0, Math.min(255, parseInt(match[2], 10))),
      b: Math.max(0, Math.min(255, parseInt(match[3], 10))),
      a: isNaN(alpha) ? 1 : Math.max(0, Math.min(1, alpha))
    };
  }

  function rgbToHsl(color) {
    var r = color.r / 255, g = color.g / 255, b = color.b / 255;
    var max = Math.max(r, g, b), min = Math.min(r, g, b);
    var h = 0, s = 0, l = (max + min) / 2;
    if (max !== min) {
      var d = max - min;
      s = l > 0.5 ? d / (2 - max - min) : d / (max + min);
      if (max === r) h = (g - b) / d + (g < b ? 6 : 0);
      else if (max === g) h = (b - r) / d + 2;
      else h = (r - g) / d + 4;
      h /= 6;
    }
    return { h: h * 360, s: s, l: l };
  }

  function hueToRgb(p, q, t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1 / 6) return p + (q - p) * 6 * t;
    if (t < 1 / 2) return q;
    if (t < 2 / 3) return p + (q - p) * (2 / 3 - t) * 6;
    return p;
  }

  function hslToRgb(h, s, l) {
    h = ((h % 360) + 360) % 360 / 360;
    if (s === 0) {
      var gray = Math.round(l * 255);
      return [gray, gray, gray];
    }
    var q = l < 0.5 ? l * (1 + s) : l + s - l * s;
    var p = 2 * l - q;
    return [
      Math.round(hueToRgb(p, q, h + 1 / 3) * 255),
      Math.round(hueToRgb(p, q, h) * 255),
      Math.round(hueToRgb(p, q, h - 1 / 3) * 255)
    ];
  }

  function cssColor(rgb, alpha) {
    return 'rgba(' + rgb[0] + ', ' + rgb[1] + ', ' + rgb[2] + ', ' + alpha + ')';
  }

  // White and other light surfaces become a very dark version of the same hue.
  function darkBackground(color) {
    if (!color || color.a < 0.05) return null;
    var hsl = rgbToHsl(color);
    if (hsl.l <= 0.30) return null;
    var l = 0.10 + (1 - hsl.l) * 0.08;
    return cssColor(hslToRgb(hsl.h, Math.min(hsl.s, 0.45), l), color.a);
  }

  // Dark text becomes a light, slightly tinted version of its original color.
  function lightForeground(color) {
    if (!color || color.a < 0.05) return null;
    var hsl = rgbToHsl(color);
    if (hsl.l >= 0.82) return null;
    var l = Math.min(0.95, 0.92 - hsl.l * 0.08);
    return cssColor(hslToRgb(hsl.h, Math.min(hsl.s, 0.55), l), color.a);
  }

  // Borders need to stay visible against the new dark surfaces.
  function darkBorder(color) {
    if (!color || color.a < 0.05) return null;
    var hsl = rgbToHsl(color);
    if (hsl.l < 0.32)
      return cssColor(hslToRgb(hsl.h, Math.min(hsl.s, 0.30), 0.38), Math.max(color.a, 0.55));
    if (hsl.l > 0.50)
      return cssColor(hslToRgb(hsl.h, Math.min(hsl.s, 0.30), 0.28), color.a);
    return null;
  }

  function remember(element, prop) {
    var saved = originalInline.get(element);
    if (!saved) {
      saved = {};
      originalInline.set(element, saved);
      trackedElements.add(element);
    }
    if (!Object.prototype.hasOwnProperty.call(saved, prop))
      saved[prop] = [element.style.getPropertyValue(prop), element.style.getPropertyPriority(prop)];
  }

  function apply(element, prop, value) {
    if (!value) return;
    if (element.style.getPropertyValue(prop) === value &&
        element.style.getPropertyPriority(prop) === 'important')
      return;
    remember(element, prop);
    element.style.setProperty(prop, value, 'important');
  }

  var SKIP_TAGS = {
    HEAD: 1, SCRIPT: 1, STYLE: 1, LINK: 1, META: 1, TITLE: 1, BASE: 1,
    NOSCRIPT: 1, TEMPLATE: 1, IMG: 1, VIDEO: 1, CANVAS: 1, IFRAME: 1,
    PICTURE: 1, SOURCE: 1, TRACK: 1, EMBED: 1, OBJECT: 1
  };

  var SVG_TAGS = {
    SVG: 1, PATH: 1, CIRCLE: 1, ELLIPSE: 1, LINE: 1, POLYGON: 1,
    POLYLINE: 1, RECT: 1, TEXT: 1, TSPAN: 1, G: 1, USE: 1
  };

  function processElement(element) {
    if (!enabled || !element || element.nodeType !== 1) return;
    var tag = element.tagName ? element.tagName.toUpperCase() : '';
    if (SKIP_TAGS[tag]) return;

    if (tag !== 'HTML' && tag !== 'BODY') {
      var rects = element.getClientRects();
      if (!rects || rects.length === 0) return;
    }

    var style;
    try { style = getComputedStyle(element); } catch (error) { return; }
    if (!style) return;

    var background = darkBackground(parseColor(style.backgroundColor));
    if (background) apply(element, 'background-color', background);

    var foreground = lightForeground(parseColor(style.color));
    if (foreground) apply(element, 'color', foreground);

    var borders = [
      ['border-top-color', 'borderTopStyle', 'borderTopWidth'],
      ['border-right-color', 'borderRightStyle', 'borderRightWidth'],
      ['border-bottom-color', 'borderBottomStyle', 'borderBottomWidth'],
      ['border-left-color', 'borderLeftStyle', 'borderLeftWidth']
    ];
    for (var i = 0; i < borders.length; i++) {
      var border = borders[i];
      if (style[border[1]] === 'none' || parseFloat(style[border[2]]) <= 0) continue;
      var borderColor = darkBorder(parseColor(style.getPropertyValue(border[0])));
      if (borderColor) apply(element, border[0], borderColor);
    }

    if (style.outlineStyle !== 'none' && parseFloat(style.outlineWidth) > 0) {
      var outlineColor = darkBorder(parseColor(style.outlineColor));
      if (outlineColor) apply(element, 'outline-color', outlineColor);
    }

    if (style.textDecorationLine && style.textDecorationLine !== 'none') {
      var decoration = darkBorder(parseColor(style.textDecorationColor));
      if (decoration) apply(element, 'text-decoration-color', decoration);
    }

    if (SVG_TAGS[tag]) {
      var fill = lightForeground(parseColor(style.getPropertyValue('fill')));
      if (fill) apply(element, 'fill', fill);
      var stroke = lightForeground(parseColor(style.getPropertyValue('stroke')));
      if (stroke) apply(element, 'stroke', stroke);
    }
  }

  function enqueueTree(root) {
    if (!enabled || !root || root.nodeType !== 1) return;
    queue.push(root);
    if (root.querySelectorAll) {
      var nodes = root.querySelectorAll('*');
      for (var i = 0; i < nodes.length; i++) queue.push(nodes[i]);
    }
    schedule();
  }

  function schedule() {
    if (scheduled || !enabled) return;
    scheduled = true;
    var run = function () {
      scheduled = false;
      var started = Date.now();
      while (queue.length && Date.now() - started < 14) {
        var node = queue.shift();
        if (node && node.isConnected) processElement(node);
      }
      if (queue.length) schedule();
    };
    if (window.requestAnimationFrame) window.requestAnimationFrame(run);
    else window.setTimeout(run, 0);
  }

  function pageIsDark() {
    var roots = [document.documentElement, document.body];
    for (var i = 0; i < roots.length; i++) {
      if (!roots[i]) continue;
      var color = parseColor(getComputedStyle(roots[i]).backgroundColor);
      if (color && color.a >= 0.5) {
        var luminance = 0.2126 * color.r + 0.7152 * color.g + 0.0722 * color.b;
        if (luminance < 70) return true;
      }
    }
    return false;
  }

  function installStyle(alreadyDark) {
    var css =
      ':root{color-scheme:dark!important}' +
      'html{background-color:#101114!important}' +
      '::selection{background:#3b5bdb!important;color:#ffffff!important}' +
      '::-webkit-scrollbar{width:14px;height:14px}' +
      '::-webkit-scrollbar-track{background:#18181b}' +
      '::-webkit-scrollbar-thumb{background:#5f6368;border:3px solid #18181b;border-radius:8px}' +
      '::-webkit-scrollbar-thumb:hover{background:#80868b}';
    if (!alreadyDark)
      css += 'body{background-color:#101114!important}';
    var style = document.getElementById(STYLE_ID);
    if (!style) {
      style = document.createElement('style');
      style.id = STYLE_ID;
      (document.head || document.documentElement).appendChild(style);
    }
    style.textContent = css;
  }

  function start() {
    if (!enabled || !document.documentElement) return;
    var alreadyDark = pageIsDark();
    installStyle(alreadyDark);
    document.documentElement.setAttribute('data-kite-dark', 'true');

    if (!alreadyDark) enqueueTree(document.documentElement);

    observer = new MutationObserver(function (mutations) {
      if (!enabled) return;
      for (var i = 0; i < mutations.length; i++) {
        var mutation = mutations[i];
        if (mutation.type === 'childList') {
          for (var j = 0; j < mutation.addedNodes.length; j++) {
            if (mutation.addedNodes[j].nodeType === 1) enqueueTree(mutation.addedNodes[j]);
          }
        } else if (mutation.type === 'attributes' && mutation.target) {
          queue.push(mutation.target);
          schedule();
        }
      }
    });
    observer.observe(document.documentElement, {
      childList: true,
      subtree: true,
      attributes: true,
      attributeFilter: ['class', 'id']
    });
  }

  function enable() {
    if (enabled) return;
    enabled = true;
    if (originalMatchMedia) window.matchMedia = patchedMatchMedia;
    if (document.readyState === 'loading') {
      document.addEventListener('DOMContentLoaded', start, { once: true });
    } else {
      start();
    }
  }

  function disable() {
    enabled = false;
    if (observer) {
      observer.disconnect();
      observer = null;
    }
    queue.length = 0;

    var style = document.getElementById(STYLE_ID);
    if (style && style.parentNode) style.parentNode.removeChild(style);
    if (document.documentElement) document.documentElement.removeAttribute('data-kite-dark');

    trackedElements.forEach(function (element) {
      var saved = originalInline.get(element);
      if (!saved) return;
      Object.keys(saved).forEach(function (prop) {
        var previous = saved[prop];
        if (previous[0]) element.style.setProperty(prop, previous[0], previous[1]);
        else element.style.removeProperty(prop);
      });
      if (!element.getAttribute('style')) element.removeAttribute('style');
    });
    trackedElements.clear();

    if (originalMatchMedia) window.matchMedia = originalMatchMedia;
  }

  window.__kiteDarkMode = { enable: enable, disable: disable };
  enable();
})();
)JS";

static const char *DARK_MODE_DISABLE_JS =
    "if (window.__kiteDarkMode) window.__kiteDarkMode.disable();";

static WebKitUserScript *dark_mode_script = nullptr;
static bool dark_script_added = false;

static void set_webview_dark_mode(WebKitWebView *web_view, bool enable) {
    webkit_web_view_evaluate_javascript(web_view, enable ? DARK_MODE_JS : DARK_MODE_DISABLE_JS, -1, NULL, NULL, NULL,
                                        NULL, NULL);
}

static void set_dark_mode_script(bool enable) {
    WebKitUserContentManager *manager = adblock_content_manager();
    if (!manager || dark_script_added == enable)
        return;

    if (enable) {
        if (!dark_mode_script)
            dark_mode_script = webkit_user_script_new(DARK_MODE_JS, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                                                      WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
        webkit_user_content_manager_add_script(manager, dark_mode_script);
    } else if (dark_mode_script) {
        webkit_user_content_manager_remove_script(manager, dark_mode_script);
    }
    dark_script_added = enable;
}

// Keep the GTK chrome (toolbar, tabs, menus) in sync with the page theme.
static void sync_native_theme(bool enable) {
    GtkSettings *gtk_settings = gtk_settings_get_default();
    if (gtk_settings) {
        g_object_set(gtk_settings, "gtk-application-prefer-dark-theme", enable, NULL);
    }

    if (browser.main_window) {
        GtkStyleContext *context = gtk_widget_get_style_context(browser.main_window);
        if (enable)
            gtk_style_context_add_class(context, "kite-dark");
        else
            gtk_style_context_remove_class(context, "kite-dark");
    }
}

static bool uri_is_internal(const gchar *uri) {
    return uri && g_str_has_prefix(uri, "kite:");
}

void apply_dark_mode(bool enable) {
    settings_set_dark_mode(enable);
    set_dark_mode_script(enable);
    sync_native_theme(enable);

    gint n_pages = gtk_notebook_get_n_pages(GTK_NOTEBOOK(browser.notebook));
    for (gint i = 0; i < n_pages; i++) {
        GtkWidget *child = gtk_notebook_get_nth_page(GTK_NOTEBOOK(browser.notebook), i);
        if (!child || !WEBKIT_IS_WEB_VIEW(child))
            continue;

        WebKitWebView *web_view = WEBKIT_WEB_VIEW(child);
        const gchar *uri = webkit_web_view_get_uri(web_view);
        if (!uri)
            uri = (const gchar *)g_object_get_data(G_OBJECT(web_view), "kite_requested_url");

        if (uri_is_internal(uri)) {
            // Internal pages generate their palette on the server side.
            webkit_web_view_reload(web_view);
        } else {
            set_webview_dark_mode(web_view, enable);
        }
    }
    save_settings();
}

void init_dark_mode() {
    if (!dark_mode_script)
        dark_mode_script = webkit_user_script_new(DARK_MODE_JS, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                                                  WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
    bool enabled = settings_dark_mode();
    if (enabled)
        set_dark_mode_script(true);
    sync_native_theme(enabled);
}

void on_dark_mode_toggled(GtkToggleButton *button, gpointer user_data) {
    apply_dark_mode(gtk_toggle_button_get_active(button));
}

void toggle_dark_mode() {
    if (!browser.btn_dark) {
        apply_dark_mode(!settings_dark_mode());
        return;
    }
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(browser.btn_dark),
                                 !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(browser.btn_dark)));
}
