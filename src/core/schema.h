#pragma once
#include <string>
#include <vector>
#include "core/datum.h"

namespace heapdb {

// One named, typed column.
struct ColumnSpec {
  std::string name;
  FieldKind kind;
};

// The shape of a table: its columns plus which column (if any) is the primary
// key. The primary key drives the B+ tree index that the planner can exploit.
struct RowLayout {
  std::vector<ColumnSpec> columns;
  int keyPos = -1;  // index into columns of the primary key, or -1

  int find(const std::string& col) const {
    for (int i = 0; i < (int)columns.size(); ++i)
      if (columns[i].name == col) return i;
    return -1;
  }

  std::size_t width() const { return columns.size(); }
  bool hasKey() const { return keyPos >= 0; }
};

}  // namespace heapdb
