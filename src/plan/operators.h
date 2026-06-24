#pragma once
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "core/datum.h"
#include "core/rid.h"
#include "engine.h"
#include "sql/ast.h"

namespace heapdb {

// A column produced by an operator, qualified by its source table so that join
// outputs can be addressed unambiguously (table.column).
struct OutCol {
  std::string table;
  std::string column;
  FieldKind kind;
};

// One tuple flowing through the operator tree. `src` is the heap address of the
// row and is only meaningful for single-table scans (it drives UPDATE/DELETE);
// join outputs carry an empty Rid.
struct Tup {
  Record cells;
  Rid src;
};

// Volcano pull operator: call pull() until it returns nothing. Each operator
// also lists its output columns so the parent can bind predicates by position.
class Operator {
 public:
  virtual ~Operator() = default;
  virtual const std::vector<OutCol>& columns() const = 0;
  virtual std::optional<Tup> pull() = 0;
  virtual std::string describe() const = 0;
};

using OpPtr = std::unique_ptr<Operator>;

// A predicate already bound to a column position in the input.
struct BoundPred {
  int col;
  Relop op;
  Cell literal;
};

// Full-table scan that surfaces only the versions visible to the transaction.
class SeqScan : public Operator {
 public:
  SeqScan(Engine& eng, const std::string& table, const TxnHandle& txn);
  const std::vector<OutCol>& columns() const override { return cols_; }
  std::optional<Tup> pull() override;
  std::string describe() const override { return "SeqScan(" + table_ + ")"; }

 private:
  std::string table_;
  std::vector<OutCol> cols_;
  std::vector<Tup> buf_;
  std::size_t at_ = 0;
};

// Primary-key equality lookup through the B+ tree: at most one visible row.
class IndexSeek : public Operator {
 public:
  IndexSeek(Engine& eng, const std::string& table, const TxnHandle& txn, Cell key);
  const std::vector<OutCol>& columns() const override { return cols_; }
  std::optional<Tup> pull() override;
  std::string describe() const override { return "IndexSeek(" + table_ + ".pk=" + key_.show() + ")"; }

 private:
  std::string table_;
  Cell key_;
  std::vector<OutCol> cols_;
  std::optional<Tup> hit_;
  bool done_ = false;
};

// Keeps only tuples satisfying every bound predicate (AND).
class Filter : public Operator {
 public:
  Filter(OpPtr child, std::vector<BoundPred> preds);
  const std::vector<OutCol>& columns() const override { return child_->columns(); }
  std::optional<Tup> pull() override;
  std::string describe() const override;

 private:
  OpPtr child_;
  std::vector<BoundPred> preds_;
};

// In-memory hash equi-join: hash the smaller side once, then probe with the
// other so each match is O(1).
class HashJoin : public Operator {
 public:
  HashJoin(OpPtr build, int buildKey, OpPtr probe, int probeKey);
  const std::vector<OutCol>& columns() const override { return cols_; }
  std::optional<Tup> pull() override;
  std::string describe() const override;

 private:
  void prime();

  OpPtr build_, probe_;
  int buildKey_, probeKey_;
  std::vector<OutCol> cols_;
  std::unordered_map<std::string, std::vector<Record>> table_;
  bool primed_ = false;
  // probe state
  std::optional<Tup> probeRow_;
  std::vector<Record>* matches_ = nullptr;
  std::size_t matchAt_ = 0;
};

// Narrows each tuple to the requested columns.
class Project : public Operator {
 public:
  Project(OpPtr child, std::vector<int> keep, std::vector<OutCol> outCols);
  const std::vector<OutCol>& columns() const override { return cols_; }
  std::optional<Tup> pull() override;
  std::string describe() const override { return "Project"; }

 private:
  OpPtr child_;
  std::vector<int> keep_;
  std::vector<OutCol> cols_;
};

}  // namespace heapdb
