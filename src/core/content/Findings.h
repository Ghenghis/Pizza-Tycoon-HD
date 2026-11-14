/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace core::content {

enum class Severity {
  Error,
  Warning,
};

// One result of reading or validating a content document: the rule id from
// location-contract.md section 5 (SCHEMA-1, ROAD-3, SCN-3, ...), where it
// applies (a JSON pointer into the document, empty for the whole document)
// and what is wrong.
struct Finding {
  std::string rule;
  Severity severity{Severity::Error};
  std::string pointer;
  std::string message;
};

using Findings = std::vector<Finding>;

[[nodiscard]] inline bool has_errors(const Findings& findings) {
  return std::any_of(findings.begin(), findings.end(), [](const Finding& f) {
    return f.severity == Severity::Error;
  });
}

[[nodiscard]] inline bool has_rule(const Findings& findings,
                                   std::string_view rule) {
  return std::any_of(findings.begin(), findings.end(),
                     [rule](const Finding& f) { return f.rule == rule; });
}

// The distinct ids of the rules that failed with an error, sorted.
[[nodiscard]] inline std::vector<std::string>
error_rules(const Findings& findings) {
  std::vector<std::string> rules;
  for (const Finding& f : findings) {
    if (f.severity == Severity::Error) {
      rules.push_back(f.rule);
    }
  }
  std::sort(rules.begin(), rules.end());
  rules.erase(std::unique(rules.begin(), rules.end()), rules.end());
  return rules;
}

// One line per finding, for logs and test output.
[[nodiscard]] inline std::string describe(const Findings& findings) {
  std::string out;
  for (const Finding& f : findings) {
    out += f.rule;
    out += f.severity == Severity::Error ? " error" : " warning";
    if (!f.pointer.empty()) {
      out += " at ";
      out += f.pointer;
    }
    out += ": ";
    out += f.message;
    out += '\n';
  }
  return out;
}

} // namespace core::content
