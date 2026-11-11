#!/usr/bin/env python3
"""Generate WebKit content-blocker rules for Kite Browser."""

import json
import sys

OUTPUT_FILE = "filters.json"

# Resource types accepted by WebKit's content-extension parser. Documents are
# deliberately excluded from the blocked set so typing or clicking a direct
# link to a blocked host still works. "raw" covers fetch, websocket, ping and
# other network loads.
BLOCKED_RESOURCE_TYPES = [
    "image",
    "style-sheet",
    "script",
    "font",
    "raw",
    "svg-document",
    "media",
    "websocket",
    "fetch",
    "ping",
    "other",
]

COSMETIC_SELECTORS = [
    ".advertisement",
    ".advertising",
    ".ad-banner",
    ".ad-container",
    ".ad-slot",
    ".adsbygoogle",
    "[id^='google_ads']",
    "[id^='div-gpt-ad']",
    ".taboola",
    ".outbrain",
    ".sponsored-content",
    ".sponsored-post",
    ".native-ad",
    # YouTube ad units not covered by the in-page ad-block script.
    "#masthead-ad",
    "#player-ads",
    "ytd-display-ad-renderer",
    "ytd-promoted-sparkles-web-renderer",
    "ytd-promoted-sparkles-text-search-renderer",
    "ytd-in-feed-ad-layout-renderer",
    "ytd-ad-slot-renderer",
    "ytd-banner-promo-renderer",
    "ytd-video-masthead-ad-v3-renderer",
    "ytd-compact-promoted-video-renderer",
    ".ytp-ad-overlay-container",
    ".ytp-ad-progress-list",
    ".ytp-ad-module",
    ".ytp-ad-player-overlay",
    ".ytp-ad-image-overlay",
    ".ytp-ad-text-overlay",
    ".ytp-ad-survey",
]

# YouTube ad traffic that is still a separate network request. Video ads are
# removed in-page by youtube_adblock.cpp because they share the normal CDN.
AD_URL_FILTERS = [
    r"^https?://([^/]+\.)?youtube\.com/api/stats/ads",
    r"^https?://([^/]+\.)?youtube\.com/pagead/",
    r"^https?://([^/]+\.)?youtube\.com/ptracking",
    r"^https?://([^/]+\.)?youtube\.com/youtubei/v1/log_event",
]

AD_DOMAINS = [
    "spclient.wg.spotify.com", "audio-ak-spotify-com.akamaized.net", "adswizz.com", "tritondigital.com",
    "imasdk.googleapis.com", "teads.tv", "moatads.com", "tremorhub.com",
    "amazon-adsystem.com", "aax.amazon-adsystem.com", "media.net", "adroll.com", "pubmatic.com",
    "criteo.com", "criteo.net", "revcontent.com", "mgid.com", "ntv.io", "nativo.com",
    "applovin.com", "admob.com", "vungle.com", "chartboost.com", "inmobi.com", "rayjump.com",
    "mintegral.com", "doubleclick.net", "googlesyndication.com", "googleadservices.com",
    "pagead2.googlesyndication.com", "taboola.com", "outbrain.com", "adnxs.com",
    "smartadserver.com", "rubiconproject.com", "openx.net", "casalemedia.com", "33across.com",
    "sharethrough.com", "spotxchange.com", "undertone.com", "yieldmo.com", "zemanta.com",
    "sizmek.com", "serving-sys.com", "flashtalking.com", "mathtag.com", "bidswitch.net",
    "adsrvr.org", "thetradedesk.com", "turn.com", "tidaltv.com", "adform.net", "yieldlab.net",
    "districtm.io", "lijit.com", "sovrn.com", "gumgum.com", "indexww.com", "popads.net",
    "propellerads.com", "revjet.com", "adkernel.com", "adotmob.com", "smadex.com",
    "startapp.com", "tapjoy.com", "unityads.unity3d.com", "ironsrc.com", "supersonicads.com",
    "ads.yahoo.com", "gemini.yahoo.com", "advertising.com", "ads.linkedin.com",
]

TRACKER_DOMAINS = [
    "google-analytics.com", "analytics.google.com", "googletagmanager.com",
    "googletagservices.com", "adservice.google.com", "scorecardresearch.com",
    "hotjar.com", "static.hotjar.com", "script.hotjar.com", "mxpnl.com", "mixpanel.com",
    "amplitude.com", "segment.com", "segment.io", "connect.facebook.net",
    "ads-twitter.com", "static.ads-twitter.com", "analytics.twitter.com", "licdn.com",
    "snap.licdn.com", "ct.pinterest.com", "analytics.tiktok.com", "ads.tiktok.com",
    "sc-static.net", "tr.snapchat.com", "bluekai.com", "crwdcntrl.net", "quantserve.com",
    "demdex.net", "omtrdc.net", "everesttech.net", "2o7.net", "krxd.net", "tapad.com",
    "liadm.com", "agkn.com", "rlcdn.com", "exelator.com", "analytics.yahoo.com",
    "geo.yahoo.com", "bat.bing.com", "clarity.ms", "statcounter.com", "fullstory.com",
    "logrocket.com", "smartlook.com", "mouseflow.com", "luckyorange.com", "crazyegg.com",
    "inspectlet.com",
]


def clean_domains(domains):
    """Lowercase, validate and deduplicate a domain list."""
    cleaned = set()
    for domain in domains:
        candidate = domain.strip().lower().lstrip(".")
        if not candidate:
            continue
        if any(not (character.isalnum() or character in ".-") for character in candidate):
            raise ValueError("invalid domain in filter list: %r" % domain)
        cleaned.add(candidate)
    return sorted(cleaned)


def domain_url_filter(domain):
    escaped = domain.replace(".", r"\.")
    # WebKit's URL-filter parser supports groups and character classes but
    # rejects disjunctions such as ([/:]|$). A prefix match catches every
    # request to the domain and its subdomains.
    return r"^https?://([^/]+\.)?" + escaped


def build_rules(ad_domains=None, tracker_domains=None, allowlist=None):
    ad_domains = clean_domains(AD_DOMAINS if ad_domains is None else ad_domains)
    tracker_domains = clean_domains(TRACKER_DOMAINS if tracker_domains is None else tracker_domains)
    allowlist = clean_domains([] if allowlist is None else allowlist)

    rules = []
    for domain in ad_domains:
        rules.append({
            "trigger": {
                "url-filter": domain_url_filter(domain),
                "resource-type": BLOCKED_RESOURCE_TYPES,
            },
            "action": {"type": "block"},
        })

    for domain in tracker_domains:
        rules.append({
            "trigger": {
                "url-filter": domain_url_filter(domain),
                "resource-type": BLOCKED_RESOURCE_TYPES,
                "load-type": ["third-party"],
            },
            "action": {"type": "block"},
        })

    for url_filter in AD_URL_FILTERS:
        rules.append({
            "trigger": {
                "url-filter": url_filter,
                "resource-type": BLOCKED_RESOURCE_TYPES,
            },
            "action": {"type": "block"},
        })

    rules.append({
        "trigger": {"url-filter": ".*"},
        "action": {
            "type": "css-display-none",
            "selector": ", ".join(COSMETIC_SELECTORS),
        },
    })

    if allowlist:
        rules.append({
            "trigger": {
                "url-filter": ".*",
                "if-domain": allowlist,
            },
            "action": {"type": "ignore-previous-rules"},
        })

    return rules


def generate(output_file=OUTPUT_FILE, ad_domains=None, tracker_domains=None, allowlist=None):
    rules = build_rules(ad_domains=ad_domains, tracker_domains=tracker_domains, allowlist=allowlist)
    with open(output_file, "w", encoding="utf-8") as handle:
        json.dump(rules, handle, indent=2)
        handle.write("\n")

    block_rules = sum(1 for rule in rules if rule["action"]["type"] == "block")
    print("[+] %s generated (%d block rules, %d total rules)" % (output_file, block_rules, len(rules)))
    return rules


def main(argv):
    output_file = argv[1] if len(argv) > 1 else OUTPUT_FILE
    generate(output_file)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
