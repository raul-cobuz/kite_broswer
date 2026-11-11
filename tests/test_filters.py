#!/usr/bin/env python3
"""Tests for update_filters.py."""

import json
import os
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

import update_filters  # noqa: E402


def find_block_rule(rules, marker):
    for rule in rules:
        if rule["action"]["type"] == "block" and marker in rule["trigger"]["url-filter"]:
            return rule
    raise AssertionError("missing block rule for %s" % marker)


def main():
    rules = update_filters.build_rules(
        ad_domains=["Ads.Test", "ads.test", "second-ads.test"],
        tracker_domains=["track.test"],
        allowlist=["safe.test"],
    )
    assert isinstance(rules, list)

    block_rules = [rule for rule in rules if rule["action"]["type"] == "block"]
    assert len(block_rules) == 3 + len(update_filters.AD_URL_FILTERS), \
        "duplicate domains must be deduplicated, special rules added once"

    ad_rule = find_block_rule(rules, r"ads\.test")
    assert ad_rule["trigger"]["url-filter"] == r"^https?://([^/]+\.)?ads\.test"
    assert "|" not in ad_rule["trigger"]["url-filter"], "WebKit rejects disjunctions"
    assert "document" not in ad_rule["trigger"]["resource-type"]
    assert "script" in ad_rule["trigger"]["resource-type"]
    assert "load-type" not in ad_rule["trigger"]

    # Keep every generated resource type inside the set WebKit accepts;
    # one unknown string rejects the entire filter list at runtime.
    webkit_resource_types = {
        "document",
        "top-document",
        "child-document",
        "image",
        "style-sheet",
        "script",
        "font",
        "raw",
        "websocket",
        "fetch",
        "other",
        "svg-document",
        "media",
        "popup",
        "ping",
        "csp-report",
    }
    assert set(update_filters.BLOCKED_RESOURCE_TYPES) <= webkit_resource_types

    tracker_rule = find_block_rule(rules, r"track\.test")
    assert tracker_rule["trigger"]["load-type"] == ["third-party"]

    youtube_rule = find_block_rule(rules, r"youtube\.com/api/stats/ads")
    assert youtube_rule["trigger"]["resource-type"] == update_filters.BLOCKED_RESOURCE_TYPES

    cosmetic_rules = [rule for rule in rules if rule["action"]["type"] == "css-display-none"]
    assert len(cosmetic_rules) == 1
    assert ".adsbygoogle" in cosmetic_rules[0]["action"]["selector"]
    assert ".ytp-ad-module" in cosmetic_rules[0]["action"]["selector"]

    allow_rules = [rule for rule in rules if rule["action"]["type"] == "ignore-previous-rules"]
    assert len(allow_rules) == 1
    assert allow_rules[0]["trigger"]["if-domain"] == ["safe.test"]
    assert rules[-1] is allow_rules[0], "allowlist must be evaluated last"

    with tempfile.TemporaryDirectory() as tmp:
        output = os.path.join(tmp, "filters.json")
        generated = update_filters.generate(
            output,
            ad_domains=["ads.test"],
            tracker_domains=["track.test"],
            allowlist=[],
        )
        with open(output, "r", encoding="utf-8") as handle:
            loaded = json.load(handle)
        assert loaded == generated
        assert not any(rule["action"]["type"] == "ignore-previous-rules" for rule in loaded)

    try:
        update_filters.clean_domains(["bad domain.test"])
    except ValueError:
        pass
    else:
        raise AssertionError("invalid domains must be rejected")

    print("filter generator tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
