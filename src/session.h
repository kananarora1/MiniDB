#pragma once
#include <optional>
#include <string>
#include "engine.h"

namespace heapdb {

// One client's view of the engine. Holds the current interactive transaction:
// statements run inside an explicit BEGIN..COMMIT, or auto-commit one at a time.
// All SQL goes through exec(), which returns the text to print.
class Session {
 public:
  explicit Session(Engine& eng) : eng_(eng) {}

  // Run exactly one statement (no trailing ';'). Throws on parse/exec errors
  // unless they are handled internally (e.g. write conflicts abort the txn).
  std::string exec(const std::string& sql);

  bool inExplicitTxn() const { return txn_.has_value(); }

 private:
  // Pick the txn to run under: the explicit one, or a fresh auto-commit txn.
  TxnHandle pickTxn(bool& autocommit);

  std::string runCreate(const struct CreateStmt& s);
  std::string runInsert(const struct InsertStmt& s);
  std::string runSelect(const struct SelectStmt& s, bool explainOnly);
  std::string runUpdate(const struct UpdateStmt& s);
  std::string runDelete(const struct DeleteStmt& s);
  std::string runTxn(const struct TxnStmt& s);
  std::string runSet(const struct SetStmt& s);

  Engine& eng_;
  std::optional<TxnHandle> txn_;
};

}  // namespace heapdb
