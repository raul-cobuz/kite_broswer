import json

OUTPUT_FILE = "filters.json"

TARGET_DOMAINS = [
    "spclient.wg.spotify.com", "audio-ak-spotify-com.akamaized.net", "adswizz.com", "tritondigital.com",
    "imasdk.googleapis.com", "teads.tv", "moatads.com", "tremorhub.com",
    "amazon-adsystem.com", "aax.amazon-adsystem.com", "media.net", "adroll.com", "pubmatic.com",
    "criteo.com", "criteo.net", "revcontent.com", "mgid.com", "ntv.io", "nativo.com",
    "applovin.com", "admob.com", "vungle.com", "chartboost.com", "inmobi.com", "rayjump.com", "mintegral.com",
    "google-analytics.com", "analytics.google.com", "googletagmanager.com", 
    "hotjar.com", "static.hotjar.com", "mxpnl.com", "mixpanel.com", "amplitude.com", "segment.com", "segment.io",
    "connect.facebook.net", "ads-twitter.com", "static.ads-twitter.com", 
    "licdn.com", "snap.licdn.com", "ct.pinterest.com", 
    "analytics.tiktok.com", "ads.tiktok.com", "sc-static.net", "tr.snapchat.com",
    "bluekai.com", "crwdcntrl.net", "quantserve.com",
    "doubleclick.net", "googlesyndication.com", "googleadservices.com",
    "pagead2.googlesyndication.com", "scorecardresearch.com", "clarity.ms",
    "taboola.com", "outbrain.com", "adnxs.com", "smartadserver.com"
]

unique_domains = sorted(set(TARGET_DOMAINS))
rules = []

for domain in unique_domains:
    escaped = domain.replace('.', '\\.')
    rules.append({
        "trigger": { "url-filter": escaped },
        "action": { "type": "block" }
    })

rules.append({
    "trigger": { "url-filter": ".*" },
    "action": {
        "type": "css-display-none",
        "selector": ".ad, .ads, .advertisement, .ad-banner, .banner-ad, [id^='google_ads'], ins.adsbygoogle, .ad-container"
    }
})

with open(OUTPUT_FILE, "w", encoding="utf-8") as f:
    json.dump(rules, f, indent=2)

print(f"[+] {OUTPUT_FILE} generat cu succes! ({len(rules)} reguli)")