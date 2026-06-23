#pragma once
#include <functional>
#include <optional>
#include <vector>
#include "core/datum.h"
#include "core/rid.h"

namespace heapdb {

// B+ tree from key (Cell) to the address (Rid) of the newest version of a row.
// Ordered, so it handles both equality and ranges, with all data in the linked
// leaves. Built in memory by scanning the heap when a table opens, so there's no
// separate on-disk index to keep in sync.
class BPlusTree {
 public:
  BPlusTree();
  ~BPlusTree();

  // Insert or replace the address for `key`.
  void upsert(const Cell& key, Rid where);

  // Remove key if present. Leaf-level removal; we don't merge under-full nodes.
  void remove(const Cell& key);

  // Exact-match lookup.
  std::optional<Rid> find(const Cell& key) const;

  // Walk keys in [lo, hi] (either bound omitted = open) in sorted order.
  void range(const std::optional<Cell>& lo, const std::optional<Cell>& hi,
             const std::function<void(const Cell&, Rid)>& fn) const;

  std::size_t size() const { return count_; }
  int height() const;

 private:
  struct Node {
    bool leaf = true;
    std::vector<Cell> keys;
    std::vector<Node*> kids;   // internal only
    std::vector<Rid> vals;     // leaf only
    Node* next = nullptr;      // leaf chain
  };

  struct Promote {
    bool split = false;
    Cell key;
    Node* right = nullptr;
  };

  Promote insertInto(Node* n, const Cell& key, Rid where);
  Node* firstLeaf() const;
  static void freeRec(Node* n);

  Node* root_;
  std::size_t count_ = 0;
  static constexpr int kMaxKeys = 32;  // fan-out; split past this
};

}  // namespace heapdb
