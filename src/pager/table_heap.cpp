#include "pager/table_heap.h"

#include <cstring>
#include <stdexcept>

namespace heapdb {

Rid TableHeap::place(const Record& row, uint64_t born, Rid older) {
  VHead h;
  h.born = born;
  h.dead = kNoTxn;
  h.olderPage = older.page;
  h.olderSlot = older.slot;

  std::string body;
  body.resize(kHeadBytes);
  std::memcpy(body.data(), &h, kHeadBytes);
  body += codec::encode(row, layout_);
  return appendBytes(body);
}

Rid TableHeap::appendBytes(const std::string& body) {
  // Try the existing pages newest-first; most inserts land on the last page.
  for (auto it = pages_.rbegin(); it != pages_.rend(); ++it) {
    Frame* f = cache_.pin(*it);
    SlottedView view(f->bytes);
    int slot = view.append(body.data(), static_cast<uint32_t>(body.size()));
    if (slot >= 0) {
      cache_.unpin(f, /*madeDirty=*/true);
      return Rid{*it, slot};
    }
    cache_.unpin(f, false);
  }
  // No room anywhere - grow a fresh page.
  int64_t page;
  Frame* f = cache_.pinNew(&page);
  SlottedView view(f->bytes);
  view.format();
  int slot = view.append(body.data(), static_cast<uint32_t>(body.size()));
  cache_.unpin(f, true);
  if (slot < 0) throw std::runtime_error("record larger than a page");
  pages_.push_back(page);
  return Rid{page, slot};
}

bool TableHeap::load(Rid at, RowVersion& out) {
  Frame* f = cache_.pin(at.page);
  SlottedView view(f->bytes);
  const char* p = nullptr;
  uint32_t len = 0;
  bool ok = view.fetch(at.slot, &p, &len);
  if (!ok) {
    cache_.unpin(f, false);
    return false;
  }
  VHead h;
  std::memcpy(&h, p, kHeadBytes);
  out.born = h.born;
  out.dead = h.dead;
  out.older = Rid{h.olderPage, h.olderSlot};
  out.self = at;
  out.fields = codec::decode(p + kHeadBytes, len - kHeadBytes, layout_);
  cache_.unpin(f, false);
  return true;
}

void TableHeap::stamp(Rid at, uint64_t dead) {
  Frame* f = cache_.pin(at.page);
  SlottedView view(f->bytes);
  const char* p = nullptr;
  uint32_t len = 0;
  if (!view.fetch(at.slot, &p, &len)) {
    cache_.unpin(f, false);
    throw std::runtime_error("stamp: slot is dead");
  }
  // Overwrite just the dead-stamp field within the header.
  char* w = view.writable(at.slot);
  VHead h;
  std::memcpy(&h, w, kHeadBytes);
  h.dead = dead;
  std::memcpy(w, &h, kHeadBytes);
  cache_.unpin(f, true);
}

void TableHeap::forEach(const std::function<void(const RowVersion&)>& fn) {
  for (int64_t page : pages_) {
    Frame* f = cache_.pin(page);
    SlottedView view(f->bytes);
    int n = view.slotCount();
    for (int s = 0; s < n; ++s) {
      const char* p = nullptr;
      uint32_t len = 0;
      if (!view.fetch(s, &p, &len)) continue;
      VHead h;
      std::memcpy(&h, p, kHeadBytes);
      RowVersion v;
      v.born = h.born;
      v.dead = h.dead;
      v.older = Rid{h.olderPage, h.olderSlot};
      v.self = Rid{page, s};
      v.fields = codec::decode(p + kHeadBytes, len - kHeadBytes, layout_);
      fn(v);
    }
    cache_.unpin(f, false);
  }
}

}  // namespace heapdb
