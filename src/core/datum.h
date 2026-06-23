#pragma once
#include <cstdint>
#include <string>
#include <stdexcept>

namespace heapdb {

// The two column types MiniDB understands. Keeping the type system tiny keeps
// the serialization format and the comparison rules short enough to defend.
enum class FieldKind : uint8_t {
  Int64 = 1,
  Text = 2,
};

// A single column value. We use a plain tagged struct rather than std::variant
// so the (de)serialization code can poke at the members directly.
struct Cell {
  FieldKind kind = FieldKind::Int64;
  int64_t num = 0;       // valid when kind == Int64
  std::string str;       // valid when kind == Text

  static Cell ofInt(int64_t v) {
    Cell c;
    c.kind = FieldKind::Int64;
    c.num = v;
    return c;
  }
  static Cell ofText(std::string v) {
    Cell c;
    c.kind = FieldKind::Text;
    c.str = std::move(v);
    return c;
  }

  // Three-way comparison: <0, 0, >0. Mixed kinds are an error - the planner
  // guarantees we only compare like with like.
  int cmp(const Cell& other) const {
    if (kind != other.kind)
      throw std::runtime_error("cannot compare values of different kinds");
    if (kind == FieldKind::Int64)
      return (num < other.num) ? -1 : (num > other.num ? 1 : 0);
    return str.compare(other.str) < 0 ? -1 : (str.compare(other.str) > 0 ? 1 : 0);
  }

  bool operator==(const Cell& o) const { return cmp(o) == 0; }
  bool operator<(const Cell& o) const { return cmp(o) < 0; }

  std::string show() const {
    return kind == FieldKind::Int64 ? std::to_string(num) : str;
  }
};

}  // namespace heapdb
