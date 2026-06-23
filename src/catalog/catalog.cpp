#include "catalog/catalog.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <stdexcept>

namespace heapdb {

namespace {
void putU32(std::string& s, uint32_t v) {
  s.append(reinterpret_cast<const char*>(&v), sizeof(v));
}
void putI64(std::string& s, int64_t v) {
  s.append(reinterpret_cast<const char*>(&v), sizeof(v));
}
uint32_t getU32(const std::string& s, std::size_t& off) {
  uint32_t v;
  std::memcpy(&v, s.data() + off, 4);
  off += 4;
  return v;
}
int64_t getI64(const std::string& s, std::size_t& off) {
  int64_t v;
  std::memcpy(&v, s.data() + off, 8);
  off += 8;
  return v;
}
}  // namespace

Catalog::Catalog(std::string metaPath) : path_(std::move(metaPath)) {}

TableMeta& Catalog::define(const std::string& name, RowLayout layout) {
  if (tables_.count(name)) throw std::runtime_error("table already exists: " + name);
  TableMeta m;
  m.name = name;
  m.layout = std::move(layout);
  auto [it, _] = tables_.emplace(name, std::move(m));
  return it->second;
}

TableMeta& Catalog::at(const std::string& name) {
  auto it = tables_.find(name);
  if (it == tables_.end()) throw std::runtime_error("no such table: " + name);
  return it->second;
}

std::vector<std::string> Catalog::names() const {
  std::vector<std::string> out;
  for (const auto& kv : tables_) out.push_back(kv.first);
  return out;
}

void Catalog::save() const {
  std::string buf;
  putU32(buf, static_cast<uint32_t>(tables_.size()));
  for (const auto& kv : tables_) {
    const TableMeta& m = kv.second;
    putU32(buf, static_cast<uint32_t>(m.name.size()));
    buf.append(m.name);
    putI64(buf, m.layout.keyPos);
    putU32(buf, static_cast<uint32_t>(m.layout.columns.size()));
    for (const ColumnSpec& c : m.layout.columns) {
      putU32(buf, static_cast<uint32_t>(c.name.size()));
      buf.append(c.name);
      buf.push_back(static_cast<char>(c.kind));
    }
    putU32(buf, static_cast<uint32_t>(m.pages.size()));
    for (int64_t p : m.pages) putI64(buf, p);
  }
  std::string tmp = path_ + ".tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) throw std::runtime_error("cannot write catalog");
  ssize_t n = ::write(fd, buf.data(), buf.size());
  (void)n;
  ::fsync(fd);
  ::close(fd);
  ::rename(tmp.c_str(), path_.c_str());  // atomic swap
}

void Catalog::load() {
  int fd = ::open(path_.c_str(), O_RDONLY);
  if (fd < 0) return;  // first boot: empty catalog
  std::string buf;
  char tmp[4096];
  ssize_t n;
  while ((n = ::read(fd, tmp, sizeof(tmp))) > 0) buf.append(tmp, n);
  ::close(fd);
  if (buf.empty()) return;

  std::size_t off = 0;
  uint32_t nt = getU32(buf, off);
  for (uint32_t i = 0; i < nt; ++i) {
    TableMeta m;
    uint32_t nl = getU32(buf, off);
    m.name.assign(buf.data() + off, nl);
    off += nl;
    m.layout.keyPos = static_cast<int>(getI64(buf, off));
    uint32_t nc = getU32(buf, off);
    for (uint32_t c = 0; c < nc; ++c) {
      ColumnSpec col;
      uint32_t cl = getU32(buf, off);
      col.name.assign(buf.data() + off, cl);
      off += cl;
      col.kind = static_cast<FieldKind>(static_cast<uint8_t>(buf[off]));
      off += 1;
      m.layout.columns.push_back(col);
    }
    uint32_t np = getU32(buf, off);
    for (uint32_t p = 0; p < np; ++p) m.pages.push_back(getI64(buf, off));
    tables_.emplace(m.name, std::move(m));
  }
}

}  // namespace heapdb
