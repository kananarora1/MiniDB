#include "session.h"

#include <cctype>
#include <sstream>
#include <stdexcept>
#include "plan/planner.h"
#include "sql/lexer.h"
#include "sql/parser.h"

namespace heapdb {

namespace {
std::string lower(std::string s) {
  for (char& c : s) c = std::tolower((unsigned char)c);
  return s;
}

// Render a result set as a light text table.
std::string renderTable(const std::vector<OutCol>& cols,
                        const std::vector<Record>& rows) {
  std::vector<std::size_t> w(cols.size());
  std::vector<std::string> head(cols.size());
  for (std::size_t i = 0; i < cols.size(); ++i) {
    head[i] = cols[i].column;
    w[i] = head[i].size();
  }
  std::vector<std::vector<std::string>> body;
  for (const Record& r : rows) {
    std::vector<std::string> line(cols.size());
    for (std::size_t i = 0; i < cols.size(); ++i) {
      line[i] = r[i].show();
      w[i] = std::max(w[i], line[i].size());
    }
    body.push_back(std::move(line));
  }
  std::ostringstream os;
  auto bar = [&]() {
    os << '+';
    for (std::size_t i = 0; i < cols.size(); ++i) os << std::string(w[i] + 2, '-') << '+';
    os << '\n';
  };
  bar();
  os << '|';
  for (std::size_t i = 0; i < cols.size(); ++i)
    os << ' ' << head[i] << std::string(w[i] - head[i].size(), ' ') << " |";
  os << '\n';
  bar();
  for (const auto& line : body) {
    os << '|';
    for (std::size_t i = 0; i < cols.size(); ++i)
      os << ' ' << line[i] << std::string(w[i] - line[i].size(), ' ') << " |";
    os << '\n';
  }
  bar();
  os << "(" << rows.size() << " row" << (rows.size() == 1 ? "" : "s") << ")";
  return os.str();
}
}  // namespace

TxnHandle Session::pickTxn(bool& autocommit) {
  if (txn_) {
    autocommit = false;
    return *txn_;
  }
  autocommit = true;
  return eng_.begin();
}

std::string Session::runCreate(const CreateStmt& s) {
  eng_.createTable(s);
  return "CREATE TABLE " + s.table;
}

std::string Session::runInsert(const InsertStmt& s) {
  bool autocommit;
  TxnHandle txn = pickTxn(autocommit);
  try {
    const RowLayout& layout = eng_.meta(s.table).layout;
    int n = 0;
    for (const auto& row : s.rows) {
      if (row.size() != layout.width())
        throw std::runtime_error("INSERT column count mismatch");
      eng_.insertRow(s.table, row, txn);
      ++n;
    }
    if (autocommit) eng_.commit(txn);
    return "INSERT " + std::to_string(n);
  } catch (DeadlockAbort&) {
    eng_.abort(txn);
    if (!autocommit) txn_.reset();
    return "ERROR: deadlock; transaction aborted";
  } catch (...) {
    eng_.abort(txn);
    if (!autocommit) txn_.reset();
    throw;
  }
}

std::string Session::runSelect(const SelectStmt& s, bool explainOnly) {
  bool autocommit;
  TxnHandle txn = pickTxn(autocommit);
  try {
    Planner planner(eng_, txn);
    Plan plan = planner.planSelect(s);
    std::string out;
    if (explainOnly) {
      out = plan.explain;
    } else {
      std::vector<Record> rows;
      while (auto t = plan.root->pull()) rows.push_back(t->cells);
      out = renderTable(plan.root->columns(), rows);
    }
    if (autocommit) eng_.commit(txn);
    return out;
  } catch (DeadlockAbort&) {
    eng_.abort(txn);
    if (!autocommit) txn_.reset();
    return "ERROR: deadlock; transaction aborted";
  } catch (...) {
    eng_.abort(txn);
    if (!autocommit) txn_.reset();
    throw;
  }
}

std::string Session::runUpdate(const UpdateStmt& s) {
  bool autocommit;
  TxnHandle txn = pickTxn(autocommit);
  try {
    const RowLayout& layout = eng_.meta(s.table).layout;
    Planner planner(eng_, txn);
    OpPtr src = planner.tableSource(s.table, s.filters, nullptr);
    std::vector<Tup> targets;
    while (auto t = src->pull()) targets.push_back(*t);
    int n = 0;
    for (Tup& t : targets) {
      Record next = t.cells;
      for (const auto& kv : s.assigns) {
        int idx = layout.find(kv.first);
        if (idx < 0) throw std::runtime_error("unknown column: " + kv.first);
        next[idx] = kv.second;
      }
      eng_.updateRow(s.table, t.src, t.cells[layout.keyPos], next, txn);
      ++n;
    }
    if (autocommit) eng_.commit(txn);
    return "UPDATE " + std::to_string(n);
  } catch (WriteConflict&) {
    eng_.abort(txn);
    if (!autocommit) txn_.reset();
    return "ERROR: write-write conflict; transaction aborted";
  } catch (DeadlockAbort&) {
    eng_.abort(txn);
    if (!autocommit) txn_.reset();
    return "ERROR: deadlock; transaction aborted";
  } catch (...) {
    eng_.abort(txn);
    if (!autocommit) txn_.reset();
    throw;
  }
}

std::string Session::runDelete(const DeleteStmt& s) {
  bool autocommit;
  TxnHandle txn = pickTxn(autocommit);
  try {
    const RowLayout& layout = eng_.meta(s.table).layout;
    Planner planner(eng_, txn);
    OpPtr src = planner.tableSource(s.table, s.filters, nullptr);
    std::vector<Tup> targets;
    while (auto t = src->pull()) targets.push_back(*t);
    int n = 0;
    for (Tup& t : targets) {
      eng_.deleteRow(s.table, t.src, t.cells[layout.keyPos], txn);
      ++n;
    }
    if (autocommit) eng_.commit(txn);
    return "DELETE " + std::to_string(n);
  } catch (WriteConflict&) {
    eng_.abort(txn);
    if (!autocommit) txn_.reset();
    return "ERROR: write-write conflict; transaction aborted";
  } catch (DeadlockAbort&) {
    eng_.abort(txn);
    if (!autocommit) txn_.reset();
    return "ERROR: deadlock; transaction aborted";
  } catch (...) {
    eng_.abort(txn);
    if (!autocommit) txn_.reset();
    throw;
  }
}

std::string Session::runTxn(const TxnStmt& s) {
  switch (s.verb) {
    case TxnVerb::Begin:
      if (txn_) throw std::runtime_error("already inside a transaction");
      txn_ = eng_.begin();
      return std::string("BEGIN (") +
             (txn_->iso == Iso::TwoPL ? "2pl" : "mvcc") + ")";
    case TxnVerb::Commit:
      if (!txn_) throw std::runtime_error("no transaction in progress");
      eng_.commit(*txn_);
      txn_.reset();
      return "COMMIT";
    case TxnVerb::Rollback:
      if (!txn_) throw std::runtime_error("no transaction in progress");
      eng_.abort(*txn_);
      txn_.reset();
      return "ROLLBACK";
  }
  return "";
}

std::string Session::runSet(const SetStmt& s) {
  if (txn_) throw std::runtime_error("cannot change isolation inside a transaction");
  eng_.setIsolation(s.mode);
  return std::string("SET isolation = ") + (s.mode == Iso::TwoPL ? "2pl" : "mvcc");
}

std::string Session::exec(const std::string& sql) {
  // Peel off a leading EXPLAIN, which we only honour for SELECT.
  bool explainOnly = false;
  std::string body = sql;
  {
    std::size_t i = 0;
    while (i < body.size() && std::isspace((unsigned char)body[i])) ++i;
    std::size_t j = i;
    while (j < body.size() && std::isalpha((unsigned char)body[j])) ++j;
    if (lower(body.substr(i, j - i)) == "explain") {
      explainOnly = true;
      body = body.substr(j);
    }
  }

  Lexer lex(body);
  Parser parser(lex.run());
  Statement st = parser.parse();

  if (auto* c = std::get_if<CreateStmt>(&st)) return runCreate(*c);
  if (auto* i = std::get_if<InsertStmt>(&st)) return runInsert(*i);
  if (auto* s = std::get_if<SelectStmt>(&st)) return runSelect(*s, explainOnly);
  if (auto* u = std::get_if<UpdateStmt>(&st)) return runUpdate(*u);
  if (auto* d = std::get_if<DeleteStmt>(&st)) return runDelete(*d);
  if (auto* t = std::get_if<TxnStmt>(&st)) return runTxn(*t);
  if (auto* z = std::get_if<SetStmt>(&st)) return runSet(*z);
  return "";
}

}  // namespace heapdb
