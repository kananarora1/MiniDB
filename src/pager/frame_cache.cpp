#include "pager/frame_cache.h"

#include <cstring>
#include <stdexcept>

namespace heapdb {

FrameCache::FrameCache(DiskStore& disk, std::size_t frames) : disk_(disk) {
  frames_.resize(frames == 0 ? 1 : frames);
}

Frame* FrameCache::pin(int64_t page) {
  auto it = resident_.find(page);
  if (it != resident_.end()) {
    Frame* f = &frames_[it->second];
    ++f->pins;
    f->hot = 1;
    ++hits_;
    return f;
  }
  ++misses_;
  Frame* f = grabVictim();
  disk_.readPage(page, f->bytes);
  f->page = page;
  f->dirty = false;
  f->pins = 1;
  f->hot = 1;
  resident_[page] = static_cast<int>(f - &frames_[0]);
  return f;
}

Frame* FrameCache::pinNew(int64_t* outPage) {
  int64_t page = disk_.growOne();
  Frame* f = pin(page);
  std::memset(f->bytes, 0, kPageBytes);
  f->dirty = true;
  *outPage = page;
  return f;
}

void FrameCache::unpin(Frame* f, bool madeDirty) {
  if (madeDirty) f->dirty = true;
  if (f->pins > 0) --f->pins;
}

void FrameCache::flush(Frame* f) {
  if (f->dirty && f->page != kNoPage) {
    disk_.writePage(f->page, f->bytes);
    f->dirty = false;
  }
}

void FrameCache::flushAll() {
  for (Frame& f : frames_) flush(&f);
  disk_.sync();
}

// Sweep the clock hand: skip pinned frames, give a touched frame one second
// chance (clear its hot bit), and evict the first cold, unpinned frame we meet.
Frame* FrameCache::grabVictim() {
  std::size_t scanned = 0;
  const std::size_t limit = frames_.size() * 2 + 1;
  while (true) {
    Frame& f = frames_[hand_];
    hand_ = (hand_ + 1) % frames_.size();
    if (f.page == kNoPage) return &f;        // never used yet
    if (f.pins == 0) {
      if (f.hot == 0) {
        flush(&f);
        resident_.erase(f.page);
        f.page = kNoPage;
        return &f;
      }
      f.hot = 0;  // second chance
    }
    if (++scanned > limit && allPinned())
      throw std::runtime_error("buffer pool exhausted: every frame is pinned");
  }
}

bool FrameCache::allPinned() const {
  for (const Frame& f : frames_)
    if (f.pins == 0) return false;
  return true;
}

}  // namespace heapdb
