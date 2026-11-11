#include "youtube_adblock.h"

#include <webkit2/webkit2.h>

#include "adblock.h"

static WebKitUserScript *youtube_script = nullptr;
static bool youtube_script_added = false;

// YouTube delivers pre-roll/mid-roll ads through the same player endpoint and
// CDN as normal videos, so a URL-based content blocker cannot tell them apart.
// This script removes the ad metadata before the player reads it, which stops
// both the ad video and the in-feed/masthead ad units. It also clicks skip
// buttons and hides ad overlays as a fallback for experiments YouTube runs
// outside the normal player response.
static const char *YOUTUBE_ADBLOCK_JS = R"JS(
(function () {
  'use strict';

  var host = (location.hostname || '').toLowerCase();
  if (!/(^|\.)youtube\.com$/.test(host) && !/(^|\.)youtube-nocookie\.com$/.test(host))
    return;
  if (window.__kiteYouTubeAdblock)
    return;
  window.__kiteYouTubeAdblock = true;

  var AD_KEYS = {
    adPlacements: 1,
    playerAds: 1,
    adSlots: 1,
    adBreakHeartbeatParams: 1,
    adBreakParams: 1,
    adThumbs: 1,
    adParams: 1,
    enforcementMessageViewModel: 1
  };

  var AD_RENDERER_KEYS = {
    adSlotRenderer: 1,
    displayAdRenderer: 1,
    promotedSparklesWebRenderer: 1,
    promotedSparklesTextSearchRenderer: 1,
    inFeedAdLayoutRenderer: 1,
    videoMastheadAdV3Renderer: 1,
    mastheadAdV3Renderer: 1,
    bannerPromoRenderer: 1,
    compactPromotedVideoRenderer: 1
  };

  var MAX_DEPTH = 24;

  function hasOwn(object, key) {
    return Object.prototype.hasOwnProperty.call(object, key);
  }

  function looksLikeAdPayload(text) {
    if (typeof text !== 'string')
      return false;
    return text.indexOf('"adPlacements"') !== -1 ||
           text.indexOf('"playerAds"') !== -1 ||
           text.indexOf('"adSlots"') !== -1 ||
           text.indexOf('"adSlotRenderer"') !== -1 ||
           text.indexOf('"promotedSparkles') !== -1 ||
           text.indexOf('"enforcementMessageViewModel"') !== -1;
  }

  function isAdRendererObject(value) {
    if (!value || typeof value !== 'object' || Array.isArray(value))
      return false;
    for (var key in AD_RENDERER_KEYS) {
      if (hasOwn(value, key))
        return true;
    }
    return false;
  }

  function stripAds(value, depth) {
    if (!value || typeof value !== 'object' || depth > MAX_DEPTH)
      return value;

    if (Array.isArray(value)) {
      for (var index = value.length - 1; index >= 0; index--) {
        if (isAdRendererObject(value[index]))
          value.splice(index, 1);
        else
          stripAds(value[index], depth + 1);
      }
      return value;
    }

    for (var adKey in AD_KEYS) {
      if (hasOwn(value, adKey)) {
        try { delete value[adKey]; } catch (error) { value[adKey] = undefined; }
      }
    }

    var keys = Object.keys(value);
    for (var i = 0; i < keys.length; i++) {
      var key = keys[i];
      var child = value[key];
      if (AD_RENDERER_KEYS[key]) {
        try { delete value[key]; } catch (error) { value[key] = undefined; }
        continue;
      }
      if (child && typeof child === 'object')
        stripAds(child, depth + 1);
    }
    return value;
  }

  function stripText(text) {
    if (!looksLikeAdPayload(text))
      return text;
    try {
      return JSON.stringify(stripAds(JSON.parse(text), 0));
    } catch (error) {
      return text;
    }
  }

  // Player JSON is sometimes parsed by YouTube with JSON.parse.
  var nativeParse = JSON.parse;
  JSON.parse = function (text, reviver) {
    var result = nativeParse.apply(this, arguments);
    if (looksLikeAdPayload(text))
      stripAds(result, 0);
    return result;
  };

  // fetch() responses (the current player endpoint).
  if (window.Response && Response.prototype) {
    var nativeJson = Response.prototype.json;
    if (nativeJson) {
      Response.prototype.json = function () {
        return nativeJson.apply(this, arguments).then(function (data) {
          return stripAds(data, 0);
        });
      };
    }

    var nativeText = Response.prototype.text;
    if (nativeText) {
      Response.prototype.text = function () {
        return nativeText.apply(this, arguments).then(stripText);
      };
    }
  }

  // Older player builds and embedded players still use XHR.
  if (window.XMLHttpRequest && XMLHttpRequest.prototype) {
    var nativeOpen = XMLHttpRequest.prototype.open;
    XMLHttpRequest.prototype.open = function (method, url) {
      try { this.__kiteRequestUrl = String(url || ''); } catch (error) {}
      return nativeOpen.apply(this, arguments);
    };

    function shouldCleanXhr(xhr) {
      var url = xhr.__kiteRequestUrl || '';
      return url.indexOf('/youtubei/') !== -1 || url.indexOf('/watch') !== -1;
    }

    var textDescriptor = Object.getOwnPropertyDescriptor(XMLHttpRequest.prototype, 'responseText');
    if (textDescriptor && textDescriptor.get) {
      Object.defineProperty(XMLHttpRequest.prototype, 'responseText', {
        configurable: true,
        enumerable: textDescriptor.enumerable,
        get: function () {
          var text = textDescriptor.get.call(this);
          if (!shouldCleanXhr(this) || !looksLikeAdPayload(text))
            return text;
          if (this.__kiteRawText === text && typeof this.__kiteCleanText === 'string')
            return this.__kiteCleanText;
          this.__kiteRawText = text;
          this.__kiteCleanText = stripText(text);
          return this.__kiteCleanText;
        }
      });
    }

    var responseDescriptor = Object.getOwnPropertyDescriptor(XMLHttpRequest.prototype, 'response');
    if (responseDescriptor && responseDescriptor.get) {
      Object.defineProperty(XMLHttpRequest.prototype, 'response', {
        configurable: true,
        enumerable: responseDescriptor.enumerable,
        get: function () {
          if (this.responseType === 'json') {
            var data = responseDescriptor.get.call(this);
            return stripAds(data, 0);
          }
          if (this.responseType === '' || this.responseType === 'text')
            return this.responseText;
          return responseDescriptor.get.call(this);
        }
      });
    }
  }

  // The first page load embeds the player response and feed data directly in
  // the HTML instead of fetching them.
  function defineStrippedProperty(name) {
    var stored;
    try {
      Object.defineProperty(window, name, {
        configurable: true,
        enumerable: true,
        get: function () { return stored; },
        set: function (value) { stored = stripAds(value, 0); }
      });
    } catch (error) {
      // A page script may have defined a non-configurable property already.
    }
  }
  defineStrippedProperty('ytInitialPlayerResponse');
  defineStrippedProperty('ytInitialData');

  var STYLE_ID = 'kite-youtube-adblock-style';
  var AD_CSS =
    '#masthead-ad,#player-ads,' +
    'ytd-display-ad-renderer,ytd-promoted-sparkles-web-renderer,' +
    'ytd-promoted-sparkles-text-search-renderer,ytd-in-feed-ad-layout-renderer,' +
    'ytd-ad-slot-renderer,ytd-banner-promo-renderer,ytd-video-masthead-ad-v3-renderer,' +
    'ytd-compact-promoted-video-renderer,ytd-action-companion-ad-renderer,' +
    '.ytp-ad-overlay-container,.ytp-ad-progress-list,.ytp-ad-module,' +
    '.ytp-ad-player-overlay,.ytp-ad-image-overlay,.ytp-ad-text-overlay,' +
    '.ytp-ad-survey,.ytp-ad-action-interstitial,.ytp-ad-skip-button-container,' +
    '.ytp-suggested-action,.ytp-featured-product,' +
    'ytd-engagement-panel-section-list-renderer[target-id="engagement-panel-ads"]' +
    '{display:none!important}';

  function installStyle() {
    if (document.getElementById(STYLE_ID))
      return;
    var root = document.head || document.documentElement;
    if (!root)
      return;
    var style = document.createElement('style');
    style.id = STYLE_ID;
    style.textContent = AD_CSS;
    root.appendChild(style);
  }

  function hideAdParents() {
    var ads = document.querySelectorAll(
      'ytd-display-ad-renderer,ytd-promoted-sparkles-web-renderer,' +
      'ytd-promoted-sparkles-text-search-renderer,ytd-in-feed-ad-layout-renderer,' +
      'ytd-ad-slot-renderer,ytd-banner-promo-renderer');
    for (var i = 0; i < ads.length; i++) {
      var ad = ads[i];
      var item = ad.closest ? ad.closest('ytd-rich-item-renderer') : null;
      if (item && item !== ad)
        item.style.setProperty('display', 'none', 'important');
    }
  }

  function skipAds() {
    var player = document.querySelector('.html5-video-player');
    if (!player || !player.classList.contains('ad-showing'))
      return;

    var button = player.querySelector(
      '.ytp-ad-skip-button,.ytp-ad-skip-button-modern,.ytp-skip-ad-button,.videoAdUiSkipButton');
    if (button) {
      button.click();
      return;
    }

    var video = player.querySelector('video');
    if (!video)
      return;
    try {
      if (video.duration && isFinite(video.duration) && video.duration > 1)
        video.currentTime = video.duration;
    } catch (error) {
      // Some ad streams are not seekable.
    }
  }

  if (document.readyState === 'loading')
    document.addEventListener('DOMContentLoaded', installStyle, { once: true });
  else
    installStyle();

  window.setInterval(skipAds, 500);
  window.setInterval(hideAdParents, 1000);
  window.setInterval(installStyle, 2000);
})();
)JS";

void youtube_adblock_set_enabled(bool enabled) {
    WebKitUserContentManager *manager = adblock_content_manager();
    if (!manager)
        return;

    if (enabled == youtube_script_added)
        return;

    if (enabled) {
        if (!youtube_script) {
            youtube_script = webkit_user_script_new(
                YOUTUBE_ADBLOCK_JS, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
        }
        webkit_user_content_manager_add_script(manager, youtube_script);
        youtube_script_added = true;
        g_print("[Kite] YouTube ad blocking enabled.\n");
    } else {
        if (youtube_script)
            webkit_user_content_manager_remove_script(manager, youtube_script);
        youtube_script_added = false;
        g_print("[Kite] YouTube ad blocking disabled.\n");
    }
}
