#pragma once
#include <cctype>
#include <string>
#include <vector>

namespace minidb {

enum class TokKind { WORD, NUMBER, STRING, SYMBOL, END };

struct Token {
  TokKind kind;
  std::string text;   // for STRING: the unquoted contents
};

// Minimal SQL tokenizer. Words (identifiers/keywords), integer numbers,
// single-quoted strings, and multi-char symbols (<=, >=, !=, <>).
class Lexer {
 public:
  explicit Lexer(const std::string &src) : s_(src) {}

  std::vector<Token> Tokenize() {
    std::vector<Token> out;
    size_t i = 0;
    while (i < s_.size()) {
      char c = s_[i];
      if (std::isspace(static_cast<unsigned char>(c))) { i++; continue; }
      if (c == ';') { i++; continue; }  // statement terminator: ignored
      // SQL line comment: "--" to end of line.
      if (c == '-' && i + 1 < s_.size() && s_[i + 1] == '-') {
        while (i < s_.size() && s_[i] != '\n') i++;
        continue;
      }
      if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
        size_t j = i;
        while (j < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[j])) || s_[j] == '_' || s_[j] == '.')) j++;
        out.push_back({TokKind::WORD, s_.substr(i, j - i)});
        i = j;
      } else if (std::isdigit(static_cast<unsigned char>(c)) ||
                 (c == '-' && i + 1 < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i + 1])))) {
        size_t j = i + 1;
        while (j < s_.size() && std::isdigit(static_cast<unsigned char>(s_[j]))) j++;
        out.push_back({TokKind::NUMBER, s_.substr(i, j - i)});
        i = j;
      } else if (c == '\'' || c == '"') {
        // String literal: accept either single or double quotes (lenient for demos).
        char quote = c;
        size_t j = i + 1;
        std::string val;
        while (j < s_.size() && s_[j] != quote) { val += s_[j]; j++; }
        out.push_back({TokKind::STRING, val});
        i = (j < s_.size()) ? j + 1 : j;
      } else {
        // multi-char operators
        if ((c == '<' || c == '>' || c == '!') && i + 1 < s_.size() &&
            (s_[i + 1] == '=' || s_[i + 1] == '>')) {
          out.push_back({TokKind::SYMBOL, s_.substr(i, 2)});
          i += 2;
        } else {
          out.push_back({TokKind::SYMBOL, std::string(1, c)});
          i++;
        }
      }
    }
    out.push_back({TokKind::END, ""});
    return out;
  }

 private:
  std::string s_;
};

}  // namespace minidb
