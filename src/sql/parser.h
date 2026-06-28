#pragma once
#include <string>
#include <vector>

#include "sql/ast.h"
#include "sql/lexer.h"

namespace minidb {

// Recursive-descent SQL parser. Produces a Statement AST or throws DBException
// with a descriptive message on a syntax error.
class Parser {
 public:
  explicit Parser(const std::string &sql);
  Statement Parse();

 private:
  const Token &Peek() const { return toks_[pos_]; }
  const Token &Next() { return toks_[pos_++]; }
  bool IsWord(const std::string &kw) const;          // case-insensitive lookahead
  bool AcceptWord(const std::string &kw);            // consume if matches
  void ExpectWord(const std::string &kw);
  void ExpectSym(const std::string &s);
  bool AcceptSym(const std::string &s);
  std::string ExpectIdent();

  Statement ParseCreate();
  Statement ParseInsert();
  Statement ParseDelete();
  Statement ParseSelect();
  Value ParseValue();
  std::vector<Predicate> ParseWhere();
  CompOp ParseCompOp();

  std::vector<Token> toks_;
  size_t pos_{0};
};

}  // namespace minidb
