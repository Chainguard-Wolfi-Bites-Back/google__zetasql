//
// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "zetasql/public/functions/json_internal.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "absl/memory/memory.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "re2/re2.h"
#include "zetasql/base/status_macros.h"

namespace zetasql {
namespace functions {
namespace json_internal {

namespace {
static LazyRE2 kKeyLexer = {"\\.([\\p{L}\\p{N}\\d_\\-\\:\\s]+)"};

static LazyRE2 kKeyRegex = {"\\.[\\p{L}\\p{N}\\d_\\-\\:\\s]+"};

static LazyRE2 kOffsetLexer = {"\\[\\s*([\\p{L}\\p{N}\\d_\\-]+)\\s*\\]"};
static LazyRE2 kOffsetRegex = {"\\[\\s*[\\p{L}\\p{N}\\d_\\-]+\\s*\\]"};

static LazyRE2 kOffsetLexerStandard = {"\\[\\s*([\\p{N}\\d_\\-]+)\\s*\\]"};
static LazyRE2 kOffsetRegexStandard = {"\\[\\s*[\\p{N}\\d_\\-]+\\s*\\]"};

static LazyRE2 kOffsetLexerStrict = {"\\[\\s*([\\p{N}\\d]+)\\s*\\]"};

// (?: alterantive-grouping) indicates a non-capuring group. Currently RE2
// does not support *+ and ++ which avoid backtracking and efficient.
// Matches strings with escaped single quotes.
static LazyRE2 kEscKeyLexer = {
    "\\[\\s*'((?:[^\\\\']|\\\\'|\\\\[^'])*)'\\s*\\]"};
static LazyRE2 kEscKeyRegex = {"\\[\\s*'(?:[^\\\\']|\\\\'|\\\\[^'])*'\\s*\\]"};

static LazyRE2 kEscKeyLexerStandard = {
    "\\.\\\"((?:[^\\\\\\\"]|\\\\\\\"|\\\\[^\\\"])*)\\\""};
static LazyRE2 kEscKeyRegexStandard = {
    "\\.\\\"(?:[^\\\\\\\"]|\\\\\\\"|\\\\[^\\\"])*\\\""};

static LazyRE2 kUnSupportedLexer = {"(\\*|\\.\\.|@)"};
static LazyRE2 kBeginRegex = {"\\$"};

constexpr char kStandardEscapeChar = '"';
constexpr char kLegacyEscapeChar = '\'';
constexpr absl::string_view kBeginToken = "";

}  // namespace

// Checks if the given JSON path is supported and valid.
absl::Status IsValidJSONPath(absl::string_view text, bool sql_standard_mode) {
  if (!RE2::Consume(&text, *kBeginRegex)) {
    return absl::OutOfRangeError("JSONPath must start with '$'");
  }

  const RE2* esc_key_regex = kEscKeyRegex.get();
  const RE2* offset_regex = kOffsetRegex.get();
  if (sql_standard_mode) {
    esc_key_regex = kEscKeyRegexStandard.get();
    offset_regex = kOffsetRegexStandard.get();
  }

  while (RE2::Consume(&text, *kKeyRegex) ||
         RE2::Consume(&text, *offset_regex) ||
         RE2::Consume(&text, *esc_key_regex)) {
  }

  // In non-standard mode, it's allowed to have a trailing dot.
  if (!text.empty() && (text != "." || sql_standard_mode)) {
    std::string token;
    bool is_unsupported = RE2::PartialMatch(text, *kUnSupportedLexer, &token);
    if (is_unsupported) {
      return absl::OutOfRangeError(
          absl::StrCat("Unsupported operator in JSONPath: ", token));
    }
    return absl::OutOfRangeError(
        absl::StrCat("Invalid token in JSONPath at: ", text));
  }
  return absl::OkStatus();
}

absl::Status IsValidJSONPathStrict(absl::string_view text) {
  if (!RE2::Consume(&text, *kBeginRegex)) {
    return absl::OutOfRangeError("JSONPath must start with '$'");
  }

  while (!text.empty()) {
    if (!RE2::Consume(&text, *kKeyRegex) &&
        !RE2::Consume(&text, *kEscKeyRegexStandard)) {
      std::string parsed_string;
      if (!RE2::Consume(&text, *kOffsetLexerStrict, &parsed_string)) {
        return absl::OutOfRangeError(
            absl::StrCat("Invalid token in JSONPath at: ", text));
      }
      int64_t index;
      if (!absl::SimpleAtoi(parsed_string, &index)) {
        return absl::OutOfRangeError(
            absl::StrCat("Invalid array index: ", parsed_string));
      }
    }
  }
  return absl::OkStatus();
}

void RemoveBackSlashFollowedByChar(std::string* token, char esc_chr) {
  if (token && !token->empty()) {
    std::string::const_iterator ritr = token->cbegin();
    std::string::iterator witr = token->begin();
    for (++ritr; ritr != token->end(); ++ritr) {
      if ((*witr != '\\' || *ritr != esc_chr)) {
        ++witr;
      }
      *witr = *ritr;
    }

    if (witr != token->end()) {
      token->erase(++witr, token->end());
    }
  }
}

// Validates and initializes a json path. During parsing, initializes all tokens
// that can be re-used by `ValidJSONPathIterator`. This avoids duplicate
// intensive regex matching.
absl::StatusOr<std::vector<std::string>> ValidateAndInitializePathTokens(
    absl::string_view path, bool sql_standard_mode) {
  if (!RE2::Consume(&path, *kBeginRegex)) {
    return absl::OutOfRangeError("JSONPath must start with '$'");
  }
  std::vector<std::string> tokens;
  tokens.push_back(std::string(kBeginToken));
  RE2* esc_key_lexer;
  RE2* offset_lexer;
  char esc_chr;
  if (sql_standard_mode) {
    esc_key_lexer = kEscKeyLexerStandard.get();
    offset_lexer = kOffsetLexerStandard.get();
    esc_chr = kStandardEscapeChar;
  } else {
    esc_key_lexer = kEscKeyLexer.get();
    offset_lexer = kOffsetLexer.get();
    esc_chr = kLegacyEscapeChar;
  }
  while (!path.empty()) {
    std::string token;
    std::string esc_token;
    if (RE2::Consume(&path, *offset_lexer, &token) ||
        RE2::Consume(&path, *kKeyLexer, &token)) {
      tokens.push_back(std::move(token));
    } else if (RE2::Consume(&path, *esc_key_lexer, &esc_token)) {
      RemoveBackSlashFollowedByChar(&esc_token, esc_chr);
      tokens.push_back(std::move(esc_token));
    } else {
      // In non-standard mode, it's allowed to have a trailing dot.
      if (path == "." && !sql_standard_mode) {
        break;
      }
      if (RE2::PartialMatch(path, *kUnSupportedLexer, &token)) {
        return absl::OutOfRangeError(
            absl::StrCat("Unsupported operator in JSONPath: ", token));
      }
      return absl::OutOfRangeError(
          absl::StrCat("Invalid token in JSONPath at: ", path));
    }
  }
  return tokens;
}

absl::StatusOr<std::unique_ptr<ValidJSONPathIterator>>
ValidJSONPathIterator::Create(absl::string_view js_path,
                              bool sql_standard_mode) {
  ZETASQL_ASSIGN_OR_RETURN(std::vector<Token> tokens,
                   ValidateAndInitializePathTokens(js_path, sql_standard_mode));
  return absl::WrapUnique(new ValidJSONPathIterator(std::move(tokens)));
}

// Validates and initializes a json path. During parsing, initializes all tokens
// that can be re-used by `StrictJSONPathIterator`. This avoids duplicate
// intensive regex matching.
absl::StatusOr<std::vector<StrictJSONPathToken>>
ValidateAndInitializeStrictPathTokens(absl::string_view path) {
  std::vector<StrictJSONPathToken> tokens;
  if (!RE2::Consume(&path, *kBeginRegex)) {
    return absl::OutOfRangeError("JSONPath must start with '$'");
  }
  tokens.push_back(StrictJSONPathToken(std::monostate()));
  while (!path.empty()) {
    std::string parsed_string;
    if (RE2::Consume(&path, *kKeyLexer, &parsed_string)) {
      StrictJSONPathToken strict_token(std::move(parsed_string));
      tokens.push_back(std::move(strict_token));
    } else if ((RE2::Consume(&path, *kOffsetLexerStrict, &parsed_string))) {
      int64_t index;
      if (!absl::SimpleAtoi(parsed_string, &index)) {
        return absl::OutOfRangeError(absl::StrCat(
            "JSONPath contains invalid array index: ", parsed_string));
      }
      StrictJSONPathToken strict_token(index);
      tokens.push_back(std::move(strict_token));
    } else if (RE2::Consume(&path, *kEscKeyLexerStandard, &parsed_string)) {
      tokens.push_back(StrictJSONPathToken(std::move(parsed_string)));
    } else {
      return absl::OutOfRangeError(
          absl::StrCat("Invalid token in JSONPath at: ", path));
    }
  }
  return tokens;
}

absl::StatusOr<std::unique_ptr<StrictJSONPathIterator>>
StrictJSONPathIterator::Create(absl::string_view path) {
  ZETASQL_ASSIGN_OR_RETURN(std::vector<StrictJSONPathToken> tokens,
                   ValidateAndInitializeStrictPathTokens(path));
  return absl::WrapUnique(new StrictJSONPathIterator(std::move(tokens)));
}

}  // namespace json_internal
}  // namespace functions
}  // namespace zetasql
