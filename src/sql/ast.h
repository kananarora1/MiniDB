#pragma once
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include "core/config.h"
#include "core/datum.h"
#include "core/schema.h"

namespace heapdb {

// Comparison operators allowed in a WHERE / ON clause.
enum class Relop { Eq, Ne, Lt, Le, Gt, Ge };

// A single "column OP literal" predicate. The column may be qualified by a table
// alias (set when a join is present).
struct Predicate {
  std::string table;   // empty if unqualified
  std::string column;
  Relop op;
  Cell literal;
};

// One item in a SELECT list: either "*" or a (possibly qualified) column.
struct Projected {
  bool star = false;
  std::string table;
  std::string column;
};

// An equi-join: FROM left JOIN right ON left.lcol = right.rcol
struct JoinClause {
  std::string rightTable;
  std::string leftTable, leftCol;
  std::string rightTableRef, rightCol;
};

struct CreateStmt {
  std::string table;
  std::vector<ColumnSpec> columns;
  int keyPos = -1;
};

struct InsertStmt {
  std::string table;
  std::vector<std::vector<Cell>> rows;
};

struct SelectStmt {
  std::vector<Projected> items;
  std::string from;
  std::optional<JoinClause> join;
  std::vector<Predicate> filters;  // ANDed together
};

struct UpdateStmt {
  std::string table;
  std::vector<std::pair<std::string, Cell>> assigns;
  std::vector<Predicate> filters;
};

struct DeleteStmt {
  std::string table;
  std::vector<Predicate> filters;
};

enum class TxnVerb { Begin, Commit, Rollback };
struct TxnStmt {
  TxnVerb verb;
};

// SET isolation = mvcc | 2pl
struct SetStmt {
  Iso mode;
};

using Statement = std::variant<CreateStmt, InsertStmt, SelectStmt, UpdateStmt,
                               DeleteStmt, TxnStmt, SetStmt>;

}  // namespace heapdb
