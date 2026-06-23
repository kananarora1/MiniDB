#pragma once
#include <cstdint>
#include <string>

namespace heapdb {

// Owns the single data file and treats it as an array of fixed-size pages
// addressed by id. The only thing that touches the filesystem for table data.
class DiskStore {
 public:
  explicit DiskStore(const std::string& path);
  ~DiskStore();

  // Read page `id` into `dst` (must hold kPageBytes). Missing pages read back
  // as zeros so a freshly grown file behaves like a formatted blank page.
  void readPage(int64_t id, char* dst);

  // Write `src` (kPageBytes) to page `id`, extending the file if needed.
  void writePage(int64_t id, const char* src);

  // Reserve and return the next page id at the end of the file.
  int64_t growOne();

  // Number of pages currently in the file.
  int64_t pageCount() const { return pages_; }

  void sync();

 private:
  int fd_ = -1;
  int64_t pages_ = 0;
};

}  // namespace heapdb
