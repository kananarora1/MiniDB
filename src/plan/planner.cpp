#include "plan/planner.h"

#include <stdexcept>

namespace heapdb {

namespace {
// Find the position of (table?, column) in an operator's output columns.
int resolve(const std::vector<OutCol>& cols, const std::string& table,
            const std::string& column) {
  int hit = -1;
  for (int i = 0; i < (int)cols.size(); ++i) {
    if (cols[i].column != column) continue;
    if (!table.empty() && cols[i].table != table) continue;
    if (hit >= 0) throw std::runtime_error("ambiguous column: " + column);
    hit = i;
  }
  if (hit < 0) throw std::runtime_error("unknown column: " + column);
  return hit;
}

// If a predicate pins the table's primary key to a constant, return that key.
std::optional<Cell> pkEquality(Engine& eng, const std::string& table,
                               const std::vector<Predicate>& filters, int* which) {
  const RowLayout& layout = eng.meta(table).layout;
  if (!layout.hasKey()) return std::nullopt;
  const std::string& pk = layout.columns[layout.keyPos].name;
  for (int i = 0; i < (int)filters.size(); ++i) {
    const Predicate& p = filters[i];
    if (p.op == Relop::Eq && p.column == pk &&
        (p.table.empty() || p.table == table)) {
      *which = i;
      return p.literal;
    }
  }
  return std::nullopt;
}
}  // namespace

OpPtr Planner::tableSource(const std::string& table,
                           const std::vector<Predicate>& filters,
                           std::string* explain) {
  std::size_t rows = eng_.estimatedRows(table);
  int pkPred = -1;
  std::optional<Cell> key = pkEquality(eng_, table, filters, &pkPred);

  OpPtr base;
  std::vector<Predicate> residual;
  if (key) {
    // Index seek wins: a primary-key equality touches at most one row, far
    // cheaper than reading all `rows`.
    base = std::make_unique<IndexSeek>(eng_, table, txn_, *key);
    for (int i = 0; i < (int)filters.size(); ++i)
      if (i != pkPred) residual.push_back(filters[i]);
    if (explain)
      *explain += "  IndexSeek " + table + " (pk=" + key->show() +
                  ", est_cost=1 vs seq=" + std::to_string(rows) + ")\n";
  } else {
    base = std::make_unique<SeqScan>(eng_, table, txn_);
    residual = filters;
    if (explain)
      *explain += "  SeqScan " + table + " (est_rows=" + std::to_string(rows) + ")\n";
  }

  if (residual.empty()) return base;

  std::vector<BoundPred> bound;
  const std::vector<OutCol>& cols = base->columns();
  for (const Predicate& p : residual)
    bound.push_back({resolve(cols, p.table, p.column), p.op, p.literal});
  if (explain)
    *explain += "  Filter (" + std::to_string(bound.size()) + " predicate(s))\n";
  return std::make_unique<Filter>(std::move(base), std::move(bound));
}

Plan Planner::planSelect(const SelectStmt& s) {
  Plan out;
  std::string ex = "QUERY PLAN\n";

  OpPtr source;
  if (!s.join) {
    source = tableSource(s.from, s.filters, &ex);
  } else {
    // Split predicates by which table they touch; unqualified ones default to
    // the left (FROM) table.
    const JoinClause& j = *s.join;
    std::vector<Predicate> leftF, rightF;
    for (const Predicate& p : s.filters) {
      if (p.table == j.rightTable)
        rightF.push_back(p);
      else
        leftF.push_back(p);
    }
    OpPtr left = tableSource(s.from, leftF, &ex);
    OpPtr right = tableSource(j.rightTable, rightF, &ex);

    int lk = resolve(left->columns(), j.leftTable, j.leftCol);
    int rk = resolve(right->columns(), j.rightTableRef, j.rightCol);

    // Build the hash table on the smaller side to keep it in memory.
    std::size_t lrows = eng_.estimatedRows(s.from);
    std::size_t rrows = eng_.estimatedRows(j.rightTable);
    if (rrows <= lrows) {
      ex += "  HashJoin (build=" + j.rightTable + ", probe=" + s.from + ")\n";
      source = std::make_unique<HashJoin>(std::move(right), rk, std::move(left), lk);
    } else {
      ex += "  HashJoin (build=" + s.from + ", probe=" + j.rightTable + ")\n";
      source = std::make_unique<HashJoin>(std::move(left), lk, std::move(right), rk);
    }
  }

  // Projection: expand "*" or pick the named columns.
  const std::vector<OutCol>& cols = source->columns();
  std::vector<int> keep;
  std::vector<OutCol> outCols;
  bool star = false;
  for (const Projected& pr : s.items)
    if (pr.star) star = true;
  if (star) {
    for (int i = 0; i < (int)cols.size(); ++i) {
      keep.push_back(i);
      outCols.push_back(cols[i]);
    }
  } else {
    for (const Projected& pr : s.items) {
      int idx = resolve(cols, pr.table, pr.column);
      keep.push_back(idx);
      outCols.push_back(cols[idx]);
    }
  }

  out.root = std::make_unique<Project>(std::move(source), std::move(keep), std::move(outCols));
  out.explain = ex;
  return out;
}

}  // namespace heapdb
