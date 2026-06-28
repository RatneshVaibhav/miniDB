#include "sql/parser.h"

#include <algorithm>

#include "common/exception.h"

namespace minidb {

static std::string Upper(const std::string &s) {
  std::string r = s;
  std::transform(r.begin(), r.end(), r.begin(), ::toupper);
  return r;
}

Parser::Parser(const std::string &sql) { toks_ = Lexer(sql).Tokenize(); }

bool Parser::IsWord(const std::string &kw) const {
  return Peek().kind == TokKind::WORD && Upper(Peek().text) == Upper(kw);
}
bool Parser::AcceptWord(const std::string &kw) {
  if (IsWord(kw)) { pos_++; return true; }
  return false;
}
void Parser::ExpectWord(const std::string &kw) {
  if (!AcceptWord(kw)) throw DBException("SQL syntax: expected '" + kw + "' but got '" + Peek().text + "'");
}
bool Parser::AcceptSym(const std::string &s) {
  if (Peek().kind == TokKind::SYMBOL && Peek().text == s) { pos_++; return true; }
  return false;
}
void Parser::ExpectSym(const std::string &s) {
  if (!AcceptSym(s)) throw DBException("SQL syntax: expected '" + s + "' but got '" + Peek().text + "'");
}
std::string Parser::ExpectIdent() {
  if (Peek().kind != TokKind::WORD) throw DBException("SQL syntax: expected identifier, got '" + Peek().text + "'");
  return Next().text;
}

Statement Parser::Parse() {
  if (AcceptWord("CREATE")) return ParseCreate();
  if (IsWord("INSERT")) return ParseInsert();
  if (IsWord("DELETE")) return ParseDelete();
  if (IsWord("SELECT")) return ParseSelect();
  if (AcceptWord("BEGIN")) { Statement s; s.type = StmtType::BEGIN; return s; }
  if (AcceptWord("COMMIT")) { Statement s; s.type = StmtType::COMMIT; return s; }
  if (AcceptWord("ABORT") || AcceptWord("ROLLBACK")) { Statement s; s.type = StmtType::ABORT; return s; }
  if (AcceptWord("EXPLAIN")) {
    Statement s; s.type = StmtType::EXPLAIN;
    s.inner = std::make_shared<Statement>(ParseSelect());
    return s;
  }
  if (AcceptWord("SHOW")) { ExpectWord("TABLES"); Statement s; s.type = StmtType::SHOW_TABLES; return s; }
  throw DBException("SQL syntax: unknown statement '" + Peek().text + "'");
}

Statement Parser::ParseCreate() {
  if (AcceptWord("TABLE")) {
    Statement s; s.type = StmtType::CREATE_TABLE;
    s.table = ExpectIdent();
    ExpectSym("(");
    do {
      ColumnDef col;
      col.name = ExpectIdent();
      std::string ty = Upper(ExpectIdent());
      col.type = StringToTypeId(ty);
      if (col.type == TypeId::INVALID) throw DBException("unknown type: " + ty);
      if (col.type == TypeId::VARCHAR && AcceptSym("(")) {
        col.length = static_cast<uint32_t>(std::stoul(Next().text));
        ExpectSym(")");
      }
      if (AcceptWord("PRIMARY")) { ExpectWord("KEY"); col.is_pk = true; }
      s.columns.push_back(col);
    } while (AcceptSym(","));
    ExpectSym(")");
    return s;
  }
  if (AcceptWord("INDEX")) {
    Statement s; s.type = StmtType::CREATE_INDEX;
    s.index_name = ExpectIdent();
    ExpectWord("ON");
    s.table = ExpectIdent();
    ExpectSym("(");
    s.index_col = ExpectIdent();
    ExpectSym(")");
    return s;
  }
  throw DBException("SQL syntax: expected TABLE or INDEX after CREATE");
}

Value Parser::ParseValue() {
  const Token &t = Peek();
  if (t.kind == TokKind::NUMBER) { Next(); return Value::BigInt(std::stoll(t.text)); }
  if (t.kind == TokKind::STRING) { Next(); return Value::Varchar(t.text); }
  if (IsWord("TRUE")) { Next(); return Value::Bool(true); }
  if (IsWord("FALSE")) { Next(); return Value::Bool(false); }
  if (IsWord("NULL")) { Next(); return Value::MakeNull(TypeId::INVALID); }
  throw DBException("SQL syntax: expected a literal, got '" + t.text + "'");
}

Statement Parser::ParseInsert() {
  ExpectWord("INSERT"); ExpectWord("INTO");
  Statement s; s.type = StmtType::INSERT;
  s.table = ExpectIdent();
  if (AcceptSym("(")) {
    do { s.insert_cols.push_back(ExpectIdent()); } while (AcceptSym(","));
    ExpectSym(")");
  }
  ExpectWord("VALUES");
  do {
    ExpectSym("(");
    std::vector<Value> row;
    do { row.push_back(ParseValue()); } while (AcceptSym(","));
    ExpectSym(")");
    s.rows.push_back(std::move(row));
  } while (AcceptSym(","));
  return s;
}

CompOp Parser::ParseCompOp() {
  const Token &t = Peek();
  if (t.kind == TokKind::SYMBOL) {
    if (t.text == "=")  { Next(); return CompOp::EQ; }
    if (t.text == "!=" || t.text == "<>") { Next(); return CompOp::NE; }
    if (t.text == "<")  { Next(); return CompOp::LT; }
    if (t.text == "<=") { Next(); return CompOp::LE; }
    if (t.text == ">")  { Next(); return CompOp::GT; }
    if (t.text == ">=") { Next(); return CompOp::GE; }
  }
  throw DBException("SQL syntax: expected comparison operator, got '" + t.text + "'");
}

std::vector<Predicate> Parser::ParseWhere() {
  std::vector<Predicate> preds;
  do {
    Predicate p;
    p.col = ExpectIdent();
    p.op = ParseCompOp();
    // RHS is either a column reference or a literal.
    if (Peek().kind == TokKind::WORD && !IsWord("TRUE") && !IsWord("FALSE") && !IsWord("NULL")) {
      p.rhs_is_col = true;
      p.rhs_col = Next().text;
    } else {
      p.rhs_is_col = false;
      p.rhs_val = ParseValue();
    }
    preds.push_back(std::move(p));
  } while (AcceptWord("AND"));
  return preds;
}

Statement Parser::ParseSelect() {
  ExpectWord("SELECT");
  Statement s; s.type = StmtType::SELECT;
  do {
    SelectItem item;
    if (AcceptSym("*")) {
      item.is_star = true;
    } else if (Peek().kind == TokKind::WORD) {
      std::string w = Upper(Peek().text);
      AggType agg = AggType::NONE;
      if (w == "COUNT") agg = AggType::COUNT;
      else if (w == "SUM") agg = AggType::SUM;
      else if (w == "MIN") agg = AggType::MIN;
      else if (w == "MAX") agg = AggType::MAX;
      else if (w == "AVG") agg = AggType::AVG;
      if (agg != AggType::NONE && toks_[pos_ + 1].kind == TokKind::SYMBOL && toks_[pos_ + 1].text == "(") {
        Next();  // fn name
        ExpectSym("(");
        if (AcceptSym("*")) { item.agg = AggType::COUNT_STAR; }
        else { item.agg = agg; item.col = ExpectIdent(); }
        ExpectSym(")");
      } else {
        item.col = ExpectIdent();
      }
    } else {
      throw DBException("SQL syntax: bad select item '" + Peek().text + "'");
    }
    if (AcceptWord("AS")) item.alias = ExpectIdent();
    s.select_items.push_back(item);
  } while (AcceptSym(","));

  ExpectWord("FROM");
  s.table = ExpectIdent();

  if (AcceptWord("JOIN")) {
    s.join.present = true;
    s.join.table = ExpectIdent();
    ExpectWord("ON");
    std::string a = ExpectIdent();
    ExpectSym("=");
    std::string b = ExpectIdent();
    // Assign join keys to their owning table (qualified or by membership resolved later).
    s.join.left_col = a;
    s.join.right_col = b;
  }

  if (AcceptWord("WHERE")) s.where = ParseWhere();

  if (AcceptWord("GROUP")) {
    ExpectWord("BY");
    do { s.group_by.push_back(ExpectIdent()); } while (AcceptSym(","));
  }
  return s;
}

Statement Parser::ParseDelete() {
  ExpectWord("DELETE"); ExpectWord("FROM");
  Statement s; s.type = StmtType::DELETE;
  s.table = ExpectIdent();
  if (AcceptWord("WHERE")) s.where = ParseWhere();
  return s;
}

}  // namespace minidb
