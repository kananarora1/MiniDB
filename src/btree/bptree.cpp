#include "btree/bptree.h"

#include <algorithm>

namespace heapdb {

BPlusTree::BPlusTree() {
  root_ = new Node();
  root_->leaf = true;
}

BPlusTree::~BPlusTree() { freeRec(root_); }

void BPlusTree::freeRec(Node* n) {
  if (!n) return;
  if (!n->leaf)
    for (Node* k : n->kids) freeRec(k);
  delete n;
}

// Lower bound within a node's key vector: first index whose key is >= `key`.
static int lowerBound(const std::vector<Cell>& keys, const Cell& key) {
  int lo = 0, hi = static_cast<int>(keys.size());
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    if (keys[mid].cmp(key) < 0)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

void BPlusTree::upsert(const Cell& key, Rid where) {
  Promote p = insertInto(root_, key, where);
  if (p.split) {
    // Root split: grow a new root one level up.
    Node* fresh = new Node();
    fresh->leaf = false;
    fresh->keys.push_back(p.key);
    fresh->kids.push_back(root_);
    fresh->kids.push_back(p.right);
    root_ = fresh;
  }
}

BPlusTree::Promote BPlusTree::insertInto(Node* n, const Cell& key, Rid where) {
  Promote res;
  if (n->leaf) {
    int i = lowerBound(n->keys, key);
    if (i < (int)n->keys.size() && n->keys[i].cmp(key) == 0) {
      n->vals[i] = where;  // replace existing
      return res;
    }
    n->keys.insert(n->keys.begin() + i, key);
    n->vals.insert(n->vals.begin() + i, where);
    ++count_;
    if ((int)n->keys.size() <= kMaxKeys) return res;

    // Split leaf: right half moves to a new leaf, copy-up the separator.
    int mid = (int)n->keys.size() / 2;
    Node* right = new Node();
    right->leaf = true;
    right->keys.assign(n->keys.begin() + mid, n->keys.end());
    right->vals.assign(n->vals.begin() + mid, n->vals.end());
    n->keys.resize(mid);
    n->vals.resize(mid);
    right->next = n->next;
    n->next = right;
    res.split = true;
    res.key = right->keys.front();
    res.right = right;
    return res;
  }

  int i = lowerBound(n->keys, key);
  // Descend into child i: keys equal to a separator go right of it.
  if (i < (int)n->keys.size() && n->keys[i].cmp(key) == 0) ++i;
  Promote down = insertInto(n->kids[i], key, where);
  if (!down.split) return res;

  n->keys.insert(n->keys.begin() + i, down.key);
  n->kids.insert(n->kids.begin() + i + 1, down.right);
  if ((int)n->keys.size() <= kMaxKeys) return res;

  // Split internal: middle key is pushed up (not copied).
  int mid = (int)n->keys.size() / 2;
  Cell up = n->keys[mid];
  Node* right = new Node();
  right->leaf = false;
  right->keys.assign(n->keys.begin() + mid + 1, n->keys.end());
  right->kids.assign(n->kids.begin() + mid + 1, n->kids.end());
  n->keys.resize(mid);
  n->kids.resize(mid + 1);
  res.split = true;
  res.key = up;
  res.right = right;
  return res;
}

std::optional<Rid> BPlusTree::find(const Cell& key) const {
  Node* n = root_;
  while (!n->leaf) {
    int i = lowerBound(n->keys, key);
    if (i < (int)n->keys.size() && n->keys[i].cmp(key) == 0) ++i;
    n = n->kids[i];
  }
  int i = lowerBound(n->keys, key);
  if (i < (int)n->keys.size() && n->keys[i].cmp(key) == 0) return n->vals[i];
  return std::nullopt;
}

void BPlusTree::remove(const Cell& key) {
  Node* n = root_;
  while (!n->leaf) {
    int i = lowerBound(n->keys, key);
    if (i < (int)n->keys.size() && n->keys[i].cmp(key) == 0) ++i;
    n = n->kids[i];
  }
  int i = lowerBound(n->keys, key);
  if (i < (int)n->keys.size() && n->keys[i].cmp(key) == 0) {
    n->keys.erase(n->keys.begin() + i);
    n->vals.erase(n->vals.begin() + i);
    --count_;
  }
}

BPlusTree::Node* BPlusTree::firstLeaf() const {
  Node* n = root_;
  while (!n->leaf) n = n->kids.front();
  return n;
}

void BPlusTree::range(const std::optional<Cell>& lo, const std::optional<Cell>& hi,
                      const std::function<void(const Cell&, Rid)>& fn) const {
  Node* n;
  if (lo) {
    n = root_;
    while (!n->leaf) {
      int i = lowerBound(n->keys, *lo);
      if (i < (int)n->keys.size() && n->keys[i].cmp(*lo) == 0) ++i;
      n = n->kids[i];
    }
  } else {
    n = firstLeaf();
  }
  while (n) {
    for (std::size_t j = 0; j < n->keys.size(); ++j) {
      if (lo && n->keys[j].cmp(*lo) < 0) continue;
      if (hi && n->keys[j].cmp(*hi) > 0) return;
      fn(n->keys[j], n->vals[j]);
    }
    n = n->next;
  }
}

int BPlusTree::height() const {
  int h = 1;
  Node* n = root_;
  while (!n->leaf) {
    ++h;
    n = n->kids.front();
  }
  return h;
}

}  // namespace heapdb
