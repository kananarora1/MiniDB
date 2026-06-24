#include "plan/operators.h"

#include "core/tuple.h"

namespace heapdb {

namespace {
bool satisfies(int three, Relop op) {
  switch (op) {
    case Relop::Eq: return three == 0;
    case Relop::Ne: return three != 0;
    case Relop::Lt: return three < 0;
    case Relop::Le: return three <= 0;
    case Relop::Gt: return three > 0;
    case Relop::Ge: return three >= 0;
  }
  return false;
}

std::vector<OutCol> columnsOf(Engine& eng, const std::string& table) {
  std::vector<OutCol> out;
  for (const ColumnSpec& c : eng.meta(table).layout.columns)
    out.push_back({table, c.name, c.kind});
  return out;
}
}  // namespace

// ----- SeqScan --------------------------------------------------------------
SeqScan::SeqScan(Engine& eng, const std::string& table, const TxnHandle& txn)
    : table_(table) {
  cols_ = columnsOf(eng, table);
  eng.scanVisible(table, txn, [&](const Record& row, Rid at) {
    buf_.push_back(Tup{row, at});
  });
}

std::optional<Tup> SeqScan::pull() {
  if (at_ >= buf_.size()) return std::nullopt;
  return buf_[at_++];
}

// ----- IndexSeek ------------------------------------------------------------
IndexSeek::IndexSeek(Engine& eng, const std::string& table, const TxnHandle& txn,
                     Cell key)
    : table_(table), key_(std::move(key)) {
  cols_ = columnsOf(eng, table);
  auto found = eng.lookupByKey(table, key_, txn);
  if (found) hit_ = Tup{found->row, found->at};
}

std::optional<Tup> IndexSeek::pull() {
  if (done_) return std::nullopt;
  done_ = true;
  return hit_;
}

// ----- Filter ---------------------------------------------------------------
Filter::Filter(OpPtr child, std::vector<BoundPred> preds)
    : child_(std::move(child)), preds_(std::move(preds)) {}

std::optional<Tup> Filter::pull() {
  while (auto t = child_->pull()) {
    bool ok = true;
    for (const BoundPred& p : preds_) {
      if (!satisfies(t->cells[p.col].cmp(p.literal), p.op)) {
        ok = false;
        break;
      }
    }
    if (ok) return t;
  }
  return std::nullopt;
}

std::string Filter::describe() const {
  return "Filter[" + std::to_string(preds_.size()) + " pred] <- " + child_->describe();
}

// ----- HashJoin -------------------------------------------------------------
HashJoin::HashJoin(OpPtr build, int buildKey, OpPtr probe, int probeKey)
    : build_(std::move(build)),
      probe_(std::move(probe)),
      buildKey_(buildKey),
      probeKey_(probeKey) {
  cols_ = build_->columns();
  for (const OutCol& c : probe_->columns()) cols_.push_back(c);
}

void HashJoin::prime() {
  while (auto t = build_->pull())
    table_[codec::encodeCell(t->cells[buildKey_])].push_back(t->cells);
  primed_ = true;
}

std::optional<Tup> HashJoin::pull() {
  if (!primed_) prime();
  while (true) {
    if (matches_ && matchAt_ < matches_->size()) {
      Tup out;
      out.cells = (*matches_)[matchAt_++];  // build-side columns first
      for (const Cell& c : probeRow_->cells) out.cells.push_back(c);
      return out;
    }
    probeRow_ = probe_->pull();
    if (!probeRow_) return std::nullopt;
    auto it = table_.find(codec::encodeCell(probeRow_->cells[probeKey_]));
    matches_ = (it == table_.end()) ? nullptr : &it->second;
    matchAt_ = 0;
  }
}

std::string HashJoin::describe() const {
  return "HashJoin(build=" + build_->describe() + ", probe=" + probe_->describe() + ")";
}

// ----- Project --------------------------------------------------------------
Project::Project(OpPtr child, std::vector<int> keep, std::vector<OutCol> outCols)
    : child_(std::move(child)), keep_(std::move(keep)), cols_(std::move(outCols)) {}

std::optional<Tup> Project::pull() {
  auto t = child_->pull();
  if (!t) return std::nullopt;
  Tup out;
  out.src = t->src;
  for (int i : keep_) out.cells.push_back(t->cells[i]);
  return out;
}

}  // namespace heapdb
