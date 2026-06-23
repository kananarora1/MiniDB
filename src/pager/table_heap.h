#pragma once
#include <cstdint>
#include <functional>
#include <vector>
#include "core/rid.h"
#include "core/schema.h"
#include "core/tuple.h"
#include "pager/frame_cache.h"
#include "pager/slotted_page.h"

namespace heapdb {

// One row version as it lives in the heap. The MVCC layer reads these and
// decides which versions a given snapshot is allowed to see.
struct RowVersion {
  uint64_t born = 0;   // xmin: txn that created this version
  uint64_t dead = 0;   // xmax: txn that superseded/deleted it (0 == still live)
  Rid older;           // link to the previous version of the same key
  Rid self;            // this version's own address
  Record fields;       // decoded column values
};

// Stores a table's rows across slotted pages. Every record carries a small
// header (born/dead/older) so one logical row can have a chain of versions,
// newest first. The heap just stores and returns versions; it never decides
// visibility - that's the MVCC layer's job.
class TableHeap {
 public:
  TableHeap(FrameCache& cache, std::vector<int64_t>& pages, const RowLayout& layout)
      : cache_(cache), pages_(pages), layout_(layout) {}

  // Append a new version; returns its address. `older` chains to the prior
  // version of the same key (noRid() for a first insert).
  Rid place(const Record& row, uint64_t born, Rid older);

  // Decode the version stored at `at`. Returns false if the slot is dead.
  bool load(Rid at, RowVersion& out);

  // Patch the dead-stamp (xmax) of an existing version in place. Same-size
  // write, so the record never moves.
  void stamp(Rid at, uint64_t dead);

  // Visit every version in the table (used by sequential scans).
  void forEach(const std::function<void(const RowVersion&)>& fn);

  const RowLayout& layout() const { return layout_; }

 private:
  // Header bytes prepended to every record body.
  struct VHead {
    uint64_t born;
    uint64_t dead;
    int64_t olderPage;
    int32_t olderSlot;
  };
  static constexpr uint32_t kHeadBytes = sizeof(VHead);

  Rid appendBytes(const std::string& body);

  FrameCache& cache_;
  std::vector<int64_t>& pages_;
  const RowLayout& layout_;
};

}  // namespace heapdb
