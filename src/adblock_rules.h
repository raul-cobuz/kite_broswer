#pragma once

#include <string>
#include <vector>

// Builds a WebKit content-blocker JSON array with one ignore-previous-rules
// entry per host. Invalid or empty hosts are skipped. Returns "[]" when no
// usable hosts remain.
std::string adblock_exception_rules_json(const std::vector<std::string> &hosts);
