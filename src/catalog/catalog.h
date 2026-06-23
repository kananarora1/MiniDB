#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "core/schema.h"

namespace heapdb {

// Everything we persist about one table: its shape and which heap pages hold its
// rows. The page list is what lets several tables share one data file.
struct TableMeta {
  std::string name;
  RowLayout layout;
  std::vector<int64_t> pages;
};

// The data dictionary: table shapes and page lists. Kept in memory and saved to
// its own small file - on every DDL (so a CREATE survives a crash) and at
// checkpoint (to capture pages tables grew into).
class Catalog {
 public:
  explicit Catalog(std::string metaPath);

  void load();
  void save() const;

  bool exists(const std::string& name) const { return tables_.count(name) > 0; }
  TableMeta& define(const std::string& name, RowLayout layout);
  TableMeta& at(const std::string& name);
  std::vector<std::string> names() const;

 private:
  std::string path_;
  std::map<std::string, TableMeta> tables_;
};

}  // namespace heapdb
