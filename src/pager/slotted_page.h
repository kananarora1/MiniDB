#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include "core/config.h"

namespace heapdb {

// A slotted page over a raw 4 KB buffer. The slot directory grows from the
// front, record bytes from the back. A row's address is (page, slot) and never
// moves, so the index and the MVCC stamps can point straight at it. Delete just
// flips a slot to a tombstone.
class SlottedView {
 public:
  static constexpr uint32_t kTombstone = 0xFFFFFFFFu;

  explicit SlottedView(char* mem) : mem_(mem) {}

  // Lay down an empty page: zero slots, free region spans the whole body.
  void format() {
    head()->slots = 0;
    head()->bodyTop = static_cast<uint32_t>(kPageBytes);
  }

  uint16_t slotCount() const { return head()->slots; }

  // Bytes available for a brand-new record (which also needs a fresh slot).
  uint32_t freeForNew() const {
    uint32_t dirEnd = sizeof(Header) + head()->slots * sizeof(Slot);
    if (head()->bodyTop <= dirEnd) return 0;
    return head()->bodyTop - dirEnd;
  }

  // Append a record. Returns the new slot number, or -1 if it will not fit.
  int append(const char* data, uint32_t len) {
    if (freeForNew() < len + sizeof(Slot)) return -1;
    Header* h = head();
    uint32_t at = h->bodyTop - len;
    std::memcpy(mem_ + at, data, len);
    Slot* s = slotAt(h->slots);
    s->offset = at;
    s->length = len;
    h->bodyTop = at;
    return h->slots++;
  }

  // Returns the record bytes for a live slot, or false for a tombstone / OOB.
  bool fetch(int slot, const char** out, uint32_t* len) const {
    if (slot < 0 || slot >= head()->slots) return false;
    const Slot* s = slotAt(slot);
    if (s->length == kTombstone) return false;
    *out = mem_ + s->offset;
    *len = s->length;
    return true;
  }

  // Mutable pointer to a live slot's bytes (used for in-place version stamps).
  char* writable(int slot) {
    const Slot* s = slotAt(slot);
    return mem_ + s->offset;
  }

  void kill(int slot) { slotAt(slot)->length = kTombstone; }

 private:
  struct Header {
    uint16_t slots;     // number of slot-directory entries
    uint16_t _pad;
    uint32_t bodyTop;   // lowest offset occupied by record bodies
  };
  struct Slot {
    uint32_t offset;
    uint32_t length;    // kTombstone means deleted
  };

  Header* head() { return reinterpret_cast<Header*>(mem_); }
  const Header* head() const { return reinterpret_cast<const Header*>(mem_); }
  Slot* slotAt(int i) {
    return reinterpret_cast<Slot*>(mem_ + sizeof(Header) + i * sizeof(Slot));
  }
  const Slot* slotAt(int i) const {
    return reinterpret_cast<const Slot*>(mem_ + sizeof(Header) + i * sizeof(Slot));
  }

  char* mem_;
};

}  // namespace heapdb
