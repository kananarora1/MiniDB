#pragma once
#include <string>
#include <vector>
#include "sql/ast.h"
#include "sql/lexer.h"

namespace heapdb {

// Recursive-descent parser for MiniDB's SQL subset. Each grammar rule is a
// small method, so the structure of the parser mirrors the structure of the
// language and is easy to step through when defending it.
class Parser {
 public:
  explicit Parser(std::vector<Token> toks) : toks_(std::move(toks)) {}

  // Parse exactly one statement (a trailing ';' is tolerated).
  Statement parse();

 private:
  const Token& cur() const { return toks_[at_]; }
  const Token& look(int n) const;
  void step() { ++at_; }

  bool isWord(const char* kw) const;
  bool isSym(const char* s) const;
  void wantWord(const char* kw);
  void wantSym(const char* s);
  std::string wantName();

  CreateStmt parseCreate();
  InsertStmt parseInsert();
  SelectStmt parseSelect();
  UpdateStmt parseUpdate();
  DeleteStmt parseDelete();
  SetStmt parseSet();

  std::vector<Predicate> parseWhere();
  Predicate parseOnePredicate();
  Cell parseLiteral();

  std::vector<Token> toks_;
  std::size_t at_ = 0;
};

}  // namespace heapdb
