#pragma once
#include <string>
#include "plan/operators.h"

namespace heapdb {

// A finished physical plan plus the human-readable EXPLAIN the optimizer chose.
struct Plan {
  OpPtr root;
  std::string explain;
};

// Cost-based planner. Estimates each table's size from its index, picks an index
// seek when there's a primary-key equality (else a full scan), and for a join
// builds the hash table on the smaller side.
class Planner {
 public:
  Planner(Engine& eng, const TxnHandle& txn) : eng_(eng), txn_(txn) {}

  Plan planSelect(const SelectStmt& s);

  // A single-table source (index seek or filtered scan) used by UPDATE/DELETE to
  // locate the rows they will modify. `explain` collects the chosen method.
  OpPtr tableSource(const std::string& table, const std::vector<Predicate>& filters,
                    std::string* explain);

 private:
  Engine& eng_;
  TxnHandle txn_;
};

}  // namespace heapdb
