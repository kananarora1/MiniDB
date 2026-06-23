#pragma once
#include <cstring>
#include <string>
#include <vector>
#include "core/datum.h"
#include "core/schema.h"

namespace heapdb {

// A row is just an ordered bag of cells, matching the table's RowLayout.
using Record = std::vector<Cell>;

// --- On-disk encoding -------------------------------------------------------
// A record is encoded as the concatenation of its fields, in column order:
//   Int64 -> 8 raw little-endian-ish bytes (memcpy of int64_t)
//   Text  -> 4-byte length prefix, then that many raw chars
// We never store the schema with the bytes; the caller supplies the layout when
// decoding. This keeps records compact at the cost of being schema-coupled.
namespace codec {

inline void putU32(std::string& out, uint32_t v) {
  out.append(reinterpret_cast<const char*>(&v), sizeof(v));
}
inline void putI64(std::string& out, int64_t v) {
  out.append(reinterpret_cast<const char*>(&v), sizeof(v));
}

inline std::string encode(const Record& row, const RowLayout& layout) {
  std::string out;
  for (std::size_t i = 0; i < layout.columns.size(); ++i) {
    const Cell& c = row[i];
    if (layout.columns[i].kind == FieldKind::Int64) {
      putI64(out, c.num);
    } else {
      putU32(out, static_cast<uint32_t>(c.str.size()));
      out.append(c.str);
    }
  }
  return out;
}

inline Record decode(const char* buf, std::size_t len, const RowLayout& layout) {
  Record row;
  row.reserve(layout.columns.size());
  std::size_t off = 0;
  for (const ColumnSpec& col : layout.columns) {
    if (col.kind == FieldKind::Int64) {
      int64_t v = 0;
      std::memcpy(&v, buf + off, sizeof(v));
      off += sizeof(v);
      row.push_back(Cell::ofInt(v));
    } else {
      uint32_t n = 0;
      std::memcpy(&n, buf + off, sizeof(n));
      off += sizeof(n);
      row.push_back(Cell::ofText(std::string(buf + off, n)));
      off += n;
    }
  }
  (void)len;
  return row;
}

inline Record decode(const std::string& buf, const RowLayout& layout) {
  return decode(buf.data(), buf.size(), layout);
}

// A single cell, self-describing (used by the WAL to record a delete's key).
inline std::string encodeCell(const Cell& c) {
  std::string out;
  out.push_back(static_cast<char>(c.kind));
  if (c.kind == FieldKind::Int64) {
    putI64(out, c.num);
  } else {
    putU32(out, static_cast<uint32_t>(c.str.size()));
    out.append(c.str);
  }
  return out;
}

inline Cell decodeCell(const std::string& buf) {
  FieldKind k = static_cast<FieldKind>(static_cast<uint8_t>(buf[0]));
  std::size_t off = 1;
  if (k == FieldKind::Int64) {
    int64_t v = 0;
    std::memcpy(&v, buf.data() + off, sizeof(v));
    return Cell::ofInt(v);
  }
  uint32_t n = 0;
  std::memcpy(&n, buf.data() + off, sizeof(n));
  off += sizeof(n);
  return Cell::ofText(std::string(buf.data() + off, n));
}

}  // namespace codec
}  // namespace heapdb
