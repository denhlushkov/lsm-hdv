// Concurrent SkipList (design doc, sections 2.1.2 and 4.2.2).
//
//   * Node heights are geometric with p = 1/4: P(h) = (1 - p) * p^(h - 1),
//     capped at kSkipListMaxHeight = 16.
//   * Writes (Insert) require external synchronization (single writer).
//   * Reads (Contains, iteration) are lock-free and may run concurrently with
//     a writer: links are published with release stores and read with acquire
//     loads, and nodes are never removed while the list is alive.
//   * Nodes live in an Arena and are freed together with it.
#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>

#include "util/arena.h"
#include "util/random.h"

namespace lsm {

inline constexpr int kSkipListMaxHeight = 16;
inline constexpr uint64_t kSkipListBranching = 4;  // p = 1 / 4

// Draws a node height from the geometric distribution described above.
inline int RandomSkipListHeight(Random* rnd) {
  int height = 1;
  while (height < kSkipListMaxHeight && rnd->OneIn(kSkipListBranching)) {
    ++height;
  }
  return height;
}

template <typename Key, class Comparator>
class SkipList {
 private:
  struct Node;

 public:
  // cmp(a, b) returns <0, 0, >0. The arena must outlive the list.
  SkipList(Comparator cmp, Arena* arena, uint64_t seed = 0xdeadbeefULL);
  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  // REQUIRES: nothing equal to key is currently in the list; external sync.
  void Insert(const Key& key);

  bool Contains(const Key& key) const;

  size_t Size() const { return size_.load(std::memory_order_acquire); }
  int CurrentMaxHeight() const {
    return max_height_.load(std::memory_order_relaxed);
  }

  // Bidirectional iterator. Iterators remain valid while new keys are
  // inserted concurrently; they may or may not observe those keys.
  class Iterator {
   public:
    explicit Iterator(const SkipList* list) : list_(list), node_(nullptr) {}

    bool Valid() const { return node_ != nullptr; }
    const Key& key() const {
      assert(Valid());
      return node_->key;
    }
    void Next() {
      assert(Valid());
      node_ = node_->Next(0);
    }
    void Prev() {
      // No back links: search for the last node that is < key.
      assert(Valid());
      node_ = list_->FindLessThan(node_->key);
      if (node_ == list_->head_) {
        node_ = nullptr;
      }
    }
    // Position at the first entry with key >= target.
    void Seek(const Key& target) {
      node_ = list_->FindGreaterOrEqual(target, nullptr);
    }
    // Position at the last entry with key <= target.
    void SeekForPrev(const Key& target) {
      Seek(target);
      if (!Valid()) {
        SeekToLast();
      } else if (list_->compare_(node_->key, target) > 0) {
        Prev();
      }
    }
    void SeekToFirst() { node_ = list_->head_->Next(0); }
    void SeekToLast() {
      node_ = list_->FindLast();
      if (node_ == list_->head_) {
        node_ = nullptr;
      }
    }

   private:
    const SkipList* list_;
    Node* node_;
  };

 private:
  Node* NewNode(const Key& key, int height);

  bool KeyIsAfterNode(const Key& key, Node* n) const {
    return n != nullptr && compare_(n->key, key) < 0;
  }
  // First node >= key, or nullptr. Fills prev[level] with the predecessor at
  // every level when prev != nullptr.
  Node* FindGreaterOrEqual(const Key& key, Node** prev) const;
  // Last node < key, or head_.
  Node* FindLessThan(const Key& key) const;
  // Last node of the list, or head_ if empty.
  Node* FindLast() const;

  Comparator const compare_;
  Arena* const arena_;
  Node* const head_;
  std::atomic<int> max_height_;
  std::atomic<size_t> size_;
  Random rnd_;  // only touched by the (single) writer
};

template <typename Key, class Comparator>
struct SkipList<Key, Comparator>::Node {
  explicit Node(const Key& k) : key(k) {}

  Key const key;

  Node* Next(int n) {
    assert(n >= 0);
    return next_[n].load(std::memory_order_acquire);
  }
  void SetNext(int n, Node* x) {
    assert(n >= 0);
    next_[n].store(x, std::memory_order_release);
  }
  Node* NoBarrierNext(int n) { return next_[n].load(std::memory_order_relaxed); }
  void NoBarrierSetNext(int n, Node* x) {
    next_[n].store(x, std::memory_order_relaxed);
  }

  // Variable length: the node is over-allocated so that next_ has `height`
  // slots. next_[0] is the lowest level.
  std::atomic<Node*> next_[1];
};

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::NewNode(
    const Key& key, int height) {
  char* const mem = arena_->AllocateAligned(
      sizeof(Node) + sizeof(std::atomic<Node*>) * static_cast<size_t>(height - 1));
  Node* node = new (mem) Node(key);
  for (int i = 1; i < height; ++i) {
    new (&node->next_[i]) std::atomic<Node*>(nullptr);
  }
  return node;
}

template <typename Key, class Comparator>
SkipList<Key, Comparator>::SkipList(Comparator cmp, Arena* arena,
                                    uint64_t seed)
    : compare_(cmp),
      arena_(arena),
      head_(NewNode(Key(), kSkipListMaxHeight)),
      max_height_(1),
      size_(0),
      rnd_(seed) {
  for (int i = 0; i < kSkipListMaxHeight; ++i) {
    head_->SetNext(i, nullptr);
  }
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node*
SkipList<Key, Comparator>::FindGreaterOrEqual(const Key& key,
                                              Node** prev) const {
  Node* x = head_;
  int level = CurrentMaxHeight() - 1;
  while (true) {
    Node* next = x->Next(level);
    if (KeyIsAfterNode(key, next)) {
      x = next;  // keep searching in this level
    } else {
      if (prev != nullptr) {
        prev[level] = x;
      }
      if (level == 0) {
        return next;
      }
      --level;  // switch to the next lower level
    }
  }
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node*
SkipList<Key, Comparator>::FindLessThan(const Key& key) const {
  Node* x = head_;
  int level = CurrentMaxHeight() - 1;
  while (true) {
    assert(x == head_ || compare_(x->key, key) < 0);
    Node* next = x->Next(level);
    if (next == nullptr || compare_(next->key, key) >= 0) {
      if (level == 0) {
        return x;
      }
      --level;
    } else {
      x = next;
    }
  }
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::FindLast()
    const {
  Node* x = head_;
  int level = CurrentMaxHeight() - 1;
  while (true) {
    Node* next = x->Next(level);
    if (next == nullptr) {
      if (level == 0) {
        return x;
      }
      --level;
    } else {
      x = next;
    }
  }
}

template <typename Key, class Comparator>
void SkipList<Key, Comparator>::Insert(const Key& key) {
  Node* prev[kSkipListMaxHeight];
  Node* x = FindGreaterOrEqual(key, prev);
  // Duplicate keys are not allowed.
  assert(x == nullptr || compare_(key, x->key) != 0);
  (void)x;

  const int height = RandomSkipListHeight(&rnd_);
  if (height > CurrentMaxHeight()) {
    for (int i = CurrentMaxHeight(); i < height; ++i) {
      prev[i] = head_;
    }
    // A concurrent reader that sees the new height before the new node is
    // linked will just find nullptr links from head_ at the new levels,
    // which is fine; hence the relaxed store.
    max_height_.store(height, std::memory_order_relaxed);
  }

  x = NewNode(key, height);
  for (int i = 0; i < height; ++i) {
    // The node is not yet visible, so its own links need no barrier; the
    // release store in prev[i]->SetNext publishes it.
    x->NoBarrierSetNext(i, prev[i]->NoBarrierNext(i));
    prev[i]->SetNext(i, x);
  }
  size_.fetch_add(1, std::memory_order_release);
}

template <typename Key, class Comparator>
bool SkipList<Key, Comparator>::Contains(const Key& key) const {
  Node* x = FindGreaterOrEqual(key, nullptr);
  return x != nullptr && compare_(key, x->key) == 0;
}

}  // namespace lsm
