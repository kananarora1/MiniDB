#include "sql/parser.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace heapdb {

namespace {
bool sameWord(const std::string& a, const char* b) {
  std::size_t n = std::strlen(b);
  if (a.size() != n) return false;
  for (std::size_t i = 0; i < n; ++i)
    if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
      return false;
  return true;
}
FieldKind kindFromWord(const std::string& w) {
  if (sameWord(w, "INT") || sameWord(w, "INTEGER") || sameWord(w, "BIGINT"))
    return FieldKind::Int64;
  if (sameWord(w, "TEXT") || sameWord(w, "VARCHAR") || sameWord(w, "STRING"))
    return FieldKind::Text;
  throw std::runtime_error("unknown column type: " + w);
}
}  // namespace

const Token& Parser::look(int n) const {
  std::size_t i = at_ + n;
  return i < toks_.size() ? toks_[i] : toks_.back();
}

bool Parser::isWord(const char* kw) const {
  return cur().kind == Lexeme::Word && sameWord(cur().text, kw);
}
bool Parser::isSym(const char* s) const {
  return cur().kind == Lexeme::Symbol && cur().text == s;
}
void Parser::wantWord(const char* kw) {
  if (!isWord(kw)) throw std::runtime_error(std::string("expected keyword ") + kw);
  step();
}
void Parser::wantSym(const char* s) {
  if (!isSym(s)) throw std::runtime_error(std::string("expected '") + s + "'");
  step();
}
std::string Parser::wantName() {
  if (cur().kind != Lexeme::Word) throw std::runtime_error("expected an identifier");
  std::string n = cur().text;
  step();
  return n;
}

Statement Parser::parse() {
  if (isWord("CREATE")) return parseCreate();
  if (isWord("INSERT")) return parseInsert();
  if (isWord("SELECT")) return parseSelect();
  if (isWord("UPDATE")) return parseUpdate();
  if (isWord("DELETE")) return parseDelete();
  if (isWord("BEGIN") || isWord("START")) {
    step();
    if (isWord("TRANSACTION")) step();
    return TxnStmt{TxnVerb::Begin};
  }
  if (isWord("COMMIT")) { step(); return TxnStmt{TxnVerb::Commit}; }
  if (isWord("ROLLBACK") || isWord("ABORT")) { step(); return TxnStmt{TxnVerb::Rollback}; }
  if (isWord("SET")) return parseSet();
  throw std::runtime_error("unrecognized statement");
}

SetStmt Parser::parseSet() {
  wantWord("SET");
  wantWord("ISOLATION");
  if (isSym("=")) step();  // optional '='
  // The value "2pl" lexes as Number(2) + Word("pl"); accept that and the word
  // forms. mvcc/snapshot -> MVCC; 2pl/twopl/locking/serializable -> 2PL.
  if (cur().kind == Lexeme::Number && cur().number == 2) {
    step();
    if (isWord("pl")) step();
    return SetStmt{Iso::TwoPL};
  }
  std::string w = wantName();
  if (sameWord(w, "mvcc") || sameWord(w, "snapshot") || sameWord(w, "si"))
    return SetStmt{Iso::Mvcc};
  if (sameWord(w, "twopl") || sameWord(w, "locking") || sameWord(w, "serializable") ||
      sameWord(w, "2pl"))
    return SetStmt{Iso::TwoPL};
  throw std::runtime_error("unknown isolation level: " + w);
}

CreateStmt Parser::parseCreate() {
  wantWord("CREATE");
  wantWord("TABLE");
  CreateStmt s;
  s.table = wantName();
  wantSym("(");
  while (true) {
    ColumnSpec col;
    col.name = wantName();
    col.kind = kindFromWord(wantName());
    if (isWord("PRIMARY")) {
      step();
      wantWord("KEY");
      s.keyPos = static_cast<int>(s.columns.size());
    }
    s.columns.push_back(col);
    if (isSym(",")) { step(); continue; }
    break;
  }
  wantSym(")");
  return s;
}

InsertStmt Parser::parseInsert() {
  wantWord("INSERT");
  wantWord("INTO");
  InsertStmt s;
  s.table = wantName();
  wantWord("VALUES");
  while (true) {
    wantSym("(");
    std::vector<Cell> row;
    while (true) {
      row.push_back(parseLiteral());
      if (isSym(",")) { step(); continue; }
      break;
    }
    wantSym(")");
    s.rows.push_back(std::move(row));
    if (isSym(",")) { step(); continue; }
    break;
  }
  return s;
}

SelectStmt Parser::parseSelect() {
  wantWord("SELECT");
  SelectStmt s;
  while (true) {
    Projected p;
    if (isSym("*")) {
      p.star = true;
      step();
    } else {
      std::string first = wantName();
      if (isSym(".")) {
        step();
        p.table = first;
        p.column = wantName();
      } else {
        p.column = first;
      }
    }
    s.items.push_back(p);
    if (isSym(",")) { step(); continue; }
    break;
  }
  wantWord("FROM");
  s.from = wantName();
  if (isWord("JOIN")) {
    step();
    JoinClause j;
    j.rightTable = wantName();
    wantWord("ON");
    // left.col = right.col
    j.leftTable = wantName();
    wantSym(".");
    j.leftCol = wantName();
    wantSym("=");
    j.rightTableRef = wantName();
    wantSym(".");
    j.rightCol = wantName();
    s.join = j;
  }
  if (isWord("WHERE")) s.filters = parseWhere();
  return s;
}

UpdateStmt Parser::parseUpdate() {
  wantWord("UPDATE");
  UpdateStmt s;
  s.table = wantName();
  wantWord("SET");
  while (true) {
    std::string col = wantName();
    wantSym("=");
    s.assigns.emplace_back(col, parseLiteral());
    if (isSym(",")) { step(); continue; }
    break;
  }
  if (isWord("WHERE")) s.filters = parseWhere();
  return s;
}

DeleteStmt Parser::parseDelete() {
  wantWord("DELETE");
  wantWord("FROM");
  DeleteStmt s;
  s.table = wantName();
  if (isWord("WHERE")) s.filters = parseWhere();
  return s;
}

std::vector<Predicate> Parser::parseWhere() {
  wantWord("WHERE");
  std::vector<Predicate> out;
  while (true) {
    out.push_back(parseOnePredicate());
    if (isWord("AND")) { step(); continue; }
    break;
  }
  return out;
}

Predicate Parser::parseOnePredicate() {
  Predicate p;
  std::string first = wantName();
  if (isSym(".")) {
    step();
    p.table = first;
    p.column = wantName();
  } else {
    p.column = first;
  }
  const std::string& op = cur().text;
  if (cur().kind != Lexeme::Symbol) throw std::runtime_error("expected comparison operator");
  if (op == "=") p.op = Relop::Eq;
  else if (op == "!=") p.op = Relop::Ne;
  else if (op == "<") p.op = Relop::Lt;
  else if (op == "<=") p.op = Relop::Le;
  else if (op == ">") p.op = Relop::Gt;
  else if (op == ">=") p.op = Relop::Ge;
  else throw std::runtime_error("bad comparison operator: " + op);
  step();
  p.literal = parseLiteral();
  return p;
}

Cell Parser::parseLiteral() {
  if (cur().kind == Lexeme::Number) {
    Cell c = Cell::ofInt(cur().number);
    step();
    return c;
  }
  if (cur().kind == Lexeme::Quoted) {
    Cell c = Cell::ofText(cur().text);
    step();
    return c;
  }
  throw std::runtime_error("expected a literal value");
}

}  // namespace heapdb
