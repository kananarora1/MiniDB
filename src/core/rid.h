#pragma once
#include <cstdint>
#include <functional>
#include "core/config.h"

namespace heapdb {

// A record identifier: which page, which slot within it. This is the stable
// physical address of one row *version* in the heap.
struct Rid {
  int64_t page = kNoPage;
  int32_t slot = -1;

  bool live() const { return page != kNoPage; }
  bool operator==(const Rid& o) const { return page == o.page && slot == o.slot; }
  bool operator!=(const Rid& o) const { return !(*this == o); }
};

inline Rid noRid() { return Rid{}; }

}  // namespace heapdb

namespace std {
template <>
struct hash<heapdb::Rid> {
  std::size_t operator()(const heapdb::Rid& r) const {
    return std::hash<int64_t>()(r.page) * 1315423911u ^ std::hash<int32_t>()(r.slot);
  }
};
}  // namespace std
