#include "pager/disk_store.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include <cstring>
#include <stdexcept>
#include "core/config.h"

namespace heapdb {

DiskStore::DiskStore(const std::string& path) {
  fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd_ < 0) throw std::runtime_error("cannot open data file: " + path);
  struct stat st;
  if (::fstat(fd_, &st) != 0) throw std::runtime_error("fstat failed");
  pages_ = st.st_size / static_cast<int64_t>(kPageBytes);
}

DiskStore::~DiskStore() {
  if (fd_ >= 0) ::close(fd_);
}

void DiskStore::readPage(int64_t id, char* dst) {
  off_t at = static_cast<off_t>(id) * static_cast<off_t>(kPageBytes);
  ssize_t n = ::pread(fd_, dst, kPageBytes, at);
  if (n < 0) throw std::runtime_error("pread failed");
  // A short read means the page lies past EOF (a just-allocated page); the rest
  // is logically zero.
  if (static_cast<std::size_t>(n) < kPageBytes)
    std::memset(dst + n, 0, kPageBytes - n);
}

void DiskStore::writePage(int64_t id, const char* src) {
  off_t at = static_cast<off_t>(id) * static_cast<off_t>(kPageBytes);
  ssize_t n = ::pwrite(fd_, src, kPageBytes, at);
  if (n != static_cast<ssize_t>(kPageBytes))
    throw std::runtime_error("pwrite failed");
  if (id >= pages_) pages_ = id + 1;
}

int64_t DiskStore::growOne() {
  int64_t id = pages_++;
  // Materialize the page on disk so the file size reflects the allocation.
  char zero[kPageBytes];
  std::memset(zero, 0, kPageBytes);
  writePage(id, zero);
  return id;
}

void DiskStore::sync() {
  if (fd_ >= 0) ::fsync(fd_);
}

}  // namespace heapdb
