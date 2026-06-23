#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>
#include "core/config.h"
#include "pager/disk_store.h"

namespace heapdb {

// A resident copy of one disk page plus its bookkeeping.
struct Frame {
  char bytes[kPageBytes];
  int64_t page = kNoPage;
  bool dirty = false;
  int pins = 0;
  uint8_t hot = 0;   // clock reference bit/counter
};

// The buffer pool. Keeps a fixed set of frames resident and evicts with a
// clock-sweep (second-chance) policy when they're all taken: O(1), no list to
// splice, and one big scan can't wipe the whole pool.
class FrameCache {
 public:
  FrameCache(DiskStore& disk, std::size_t frames);

  // Pin and return the frame holding `page`, loading it if necessary.
  Frame* pin(int64_t page);

  // Allocate a fresh page on disk and return it already pinned and formatted-by
  // the caller's choosing (bytes are zeroed).
  Frame* pinNew(int64_t* outPage);

  // Release a previously pinned frame; `madeDirty` ORs into the dirty flag.
  void unpin(Frame* f, bool madeDirty);

  void flush(Frame* f);
  void flushAll();

  // Stats for the benchmark / demo.
  uint64_t hits() const { return hits_; }
  uint64_t misses() const { return misses_; }

 private:
  Frame* grabVictim();
  bool allPinned() const;

  DiskStore& disk_;
  std::vector<Frame> frames_;
  std::unordered_map<int64_t, int> resident_;  // page -> frame index
  std::size_t hand_ = 0;
  uint64_t hits_ = 0;
  uint64_t misses_ = 0;
};

}  // namespace heapdb
