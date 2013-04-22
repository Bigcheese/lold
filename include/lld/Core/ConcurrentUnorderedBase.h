//===- ConcurrentUnorderedBase.h - Shared Split Ordered List code ---------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief Implements a Split Ordered List based lock-free unordered data
///   structures.
///
/// The interface is modeled after the n3425 proposal:
/// http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2012/n3425.html
///
/// Paper:
/// http://dl.acm.org/citation.cfm?id=1147958
///
//===----------------------------------------------------------------------===//

#ifndef LLD_CORE_CONCURRENT_UNORDERED_BASE_H
#define LLD_CORE_CONCURRENT_UNORDERED_BASE_H

#include "lld/Core/Parallel.h"

namespace lld {
/// \brief A lock-free forward list sorted by split order.
template <class NodeT> class SplitOrderedList {
public:
  class iterator {
  protected:
    NodeT *_node;

  public:
    typedef NodeT *value_type;
    typedef std::forward_iterator_tag iterator_category;
    typedef value_type &reference;
    typedef value_type pointer;

    iterator() : _node(nullptr) {}
    iterator(NodeT *n) : _node(n) {}
    iterator(const iterator &other) : _node(other._node) {}

    iterator &operator++() {
      _node = _node->_next.load(std::memory_order_acquire);
      return *this;
    }

    iterator operator++(int) {
      auto tmp = *this;
      ++*this;
      return tmp;
    }

    reference operator*() { return _node; }

    pointer operator->() { return _node; }

    bool operator==(const iterator &other) const {
      return _node == other._node;
    }

    bool operator!=(const iterator &other) const { return !(*this == other); }

    iterator &operator=(const iterator & other) {
      _node = other._node;
      return *this;
    }

    operator NodeT *() const { return _node; }
  };

  NodeT * _head;

  template <class Alloc> SplitOrderedList(const Alloc &a) {
    _head = new (Alloc(a).allocate(1)) NodeT(0);
  }

  /// \returns the last Node less than or equal to key and the node after it.
  std::pair<std::pair<iterator, iterator>, bool>
  find(NodeT *start, std::size_t key) {
    NodeT *prev = start;
    NodeT *cur = start;
    while (true) {
      if (!cur)
        return std::make_pair(std::make_pair(prev, cur), false);
      auto next = cur->_next.load(std::memory_order_acquire);
      auto ckey = cur->_key;
      if (ckey == key)
        return std::make_pair(std::make_pair(prev, cur), true);
      if (ckey > key)
        return std::make_pair(std::make_pair(prev, cur), false);
      prev = cur;
      cur = next;
    }
  }

  NodeT *insert(NodeT *prev, NodeT *newNode, NodeT *cur) {
    newNode->_next.store(cur);
    if (prev->_next.compare_exchange_strong(cur, newNode))
      return newNode;
    return cur;
  }

  std::pair<iterator, bool> insert(NodeT *node) {
    return insert(_head, node);
  }

  std::pair<iterator, bool> insert(NodeT *start, NodeT *node) {
    auto key = node->_key;
    while (true) {
      auto loc = find(start, key);
      if (loc.second) // The key was already present.
        return std::make_pair(loc.first.second, false);
      if (insert(loc.first.first, node, loc.first.second) == node)
        return std::make_pair(node, true);
      // Start searching from last known less than node.
      start = loc.first.first;
    }
  }
};

namespace detail {
  /// \brief Macro compressed bit reversal table for 256 bits.
  ///
  /// http://graphics.stanford.edu/~seander/bithacks.html#BitReverseTable
  static const unsigned char bitReverseTable256[256] = {
  #define R2(n) n, n + 2 * 64, n + 1 * 64, n + 3 * 64
  #define R4(n) R2(n), R2(n + 2 * 16), R2(n + 1 * 16), R2(n + 3 * 16)
  #define R6(n) R4(n), R4(n + 2 * 4), R4(n + 1 * 4), R4(n + 3 * 4)
    R6(0), R6(2), R6(1), R6(3)
  };

  static std::size_t reverseBits(std::size_t value) {
    unsigned char in[sizeof(value)];
    unsigned char out[sizeof(value)];
    std::memcpy(in, &value, sizeof(value));
    for (unsigned i = 0; i < sizeof(value); ++i)
      out[(sizeof(value) - i) - 1] = bitReverseTable256[in[i]];
    std::memcpy(&value, out, sizeof(value));
    return value;
  }
}
} // end namespace lld

#endif
