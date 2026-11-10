#include "adblock_rules.h"

#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;

static void expect_eq(const std::string &actual, const std::string &expected, const char *label) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL %s\n  expected: %s\n  actual:   %s\n", label, expected.c_str(), actual.c_str());
        failures++;
    }
}

static std::string one_rule(const char *host) {
    return std::string("{\"trigger\":{\"url-filter\":\".*\",\"if-domain\":[\"") + host +
           "\"]},\"action\":{\"type\":\"ignore-previous-rules\"}}";
}

int main() {
    expect_eq(adblock_exception_rules_json({}), "[]", "no hosts produces an empty array");

    expect_eq(adblock_exception_rules_json({"example.com"}), "[" + one_rule("example.com") + "]",
              "single host rule");

    expect_eq(adblock_exception_rules_json({"Example.COM", "example.com"}), "[" + one_rule("example.com") + "]",
              "hosts are lowercased and deduplicated");

    expect_eq(adblock_exception_rules_json({"a.test", "b.test", "a.test"}),
              "[" + one_rule("a.test") + "," + one_rule("b.test") + "]", "hosts are sorted");

    expect_eq(adblock_exception_rules_json({"", "bad host", "quote\"host", "127.0.0.1"}),
              "[" + one_rule("127.0.0.1") + "]", "invalid hosts are skipped");

    expect_eq(adblock_exception_rules_json({"::1"}), "[" + one_rule("::1") + "]", "IPv6 hosts are kept");

    if (failures) {
        std::fprintf(stderr, "%d adblock rule test(s) failed\n", failures);
        return 1;
    }
    std::printf("adblock rule tests passed\n");
    return 0;
}
