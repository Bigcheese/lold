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

#ifdef _MSC_VER
#define LLD_REBIND_ALLOC(T) alloc_traits::template rebind_alloc<T >::other
#else
#define LLD_REBIND_ALLOC(T) alloc_traits::template rebind_alloc<T>
#endif

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

  NodeT *_head;

  template <class Alloc> SplitOrderedList(const Alloc &a) {
    typedef std::allocator_traits<Alloc> alloc_traits;
    typedef typename LLD_REBIND_ALLOC(NodeT) NodeAlloc;
    typedef std::allocator_traits<NodeAlloc> NodeAllocTraits;
    auto alloc = NodeAlloc(a);
    _head = NodeAllocTraits::allocate(alloc, 1);
    NodeAllocTraits::construct(alloc, _head, 0);
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

/// \brief Base class for ConcurrentUnordered{Map,Set}.
template <class Traits>
class ConcurrentUnorderedBase {
public:
  typedef typename Traits::key_type key_type;
  typedef typename Traits::value_type value_type;
  typedef typename Traits::hasher hasher;
  typedef typename Traits::key_equal key_equal;
  typedef typename Traits::allocator_type allocator_type;

  typedef std::allocator_traits<allocator_type> alloc_traits;

  typedef typename alloc_traits::pointer pointer;
  typedef typename alloc_traits::value_type &reference;
  typedef typename alloc_traits::size_type size_type;
  typedef typename alloc_traits::difference_type difference_type;

private:
  struct NodeBase {
    NodeBase(size_type key) : _key(key), _next(nullptr) {}
    size_type _key;
    std::atomic<NodeBase *> _next;
  };

  struct Node : NodeBase {
    Node(size_type key, value_type val)
        : NodeBase(key), _value(std::move(val)) {}

    value_type _value;
  };

  // Rebound allocators.
  typedef typename LLD_REBIND_ALLOC(NodeBase) NodeBaseAlloc;
  typedef typename LLD_REBIND_ALLOC(Node) NodeAlloc;
  typedef typename LLD_REBIND_ALLOC(std::atomic<NodeBase *>) BucketAlloc;
  typedef std::allocator_traits<NodeBaseAlloc> NodeBaseAllocTraits;
  typedef std::allocator_traits<NodeAlloc> NodeAllocTraits;
  typedef std::allocator_traits<BucketAlloc> BucketAllocTraits;

  /// \brief The size of the segment table needs to be large enough to cover
  ///   entire address range.
  static const size_type segmentTableSize = sizeof(void *) * CHAR_BIT;

  static const size_type maxLoadFactor = 2;

  /// \brief The split ordered list of nodes.
  SplitOrderedList<NodeBase> _list;

  /// \brief Each segment contains i == 0 ? 2 : 2^i buckets.
  std::atomic<std::atomic<NodeBase *> *> _segments[segmentTableSize];

  /// \brief The number of real nodes currently stored.
  std::atomic<size_type> _count;

  /// \brief The current number of buckets.
  std::atomic<size_type> _size;

  hasher _hasher;
  key_equal _equal;
  allocator_type _alloc;

  /// \brief Doesn't skip dummy keys.
  typedef typename SplitOrderedList<NodeBase>::iterator full_iterator;

public:
  class iterator : public full_iterator {
  public:
    typedef std::forward_iterator_tag iterator_category;
    typedef ptrdiff_t difference_type;
    typedef typename ConcurrentUnorderedBase::value_type value_type;
    typedef value_type &reference;
    typedef value_type *pointer;

    iterator() {}
    iterator(NodeBase *n) : full_iterator(n) {}
    iterator(const iterator &other) : full_iterator(other) {}

    iterator &operator++() {
      do {
        this->_node = this->_node->_next.load(std::memory_order_acquire);
      } while (this->_node && !(this->_node->_key & 1u));
      return *this;
    }

    iterator operator++(int) {
      auto tmp = *this;
      ++*this;
      return tmp;
    }

    reference operator*() {
      assert((this->_node->_key & 1u) && "Dereferenced dummy node!");
      return static_cast<Node *>(this->_node)->_value;
    }

    pointer operator->() {
      assert((this->_node->_key & 1u) && "Dereferenced dummy node!");
      return &static_cast<Node *>(this->_node)->_value;
    }
  };

  ConcurrentUnorderedBase(size_type n = 8, const hasher &h = hasher(),
                          const key_equal &ke = key_equal(),
                          const allocator_type &a = allocator_type())
      : _list(NodeBaseAlloc(a)), _segments(), _count(0), _size(n), _hasher(h),
        _equal(ke), _alloc(a) {
    for (unsigned i = 0; i < segmentTableSize; ++i)
      _segments[i].store(nullptr, std::memory_order_relaxed);
    setBucket(0, _list._head);
  }

  ~ConcurrentUnorderedBase() {
    for (unsigned i = 0; i < segmentTableSize; ++i) {
      auto buckets = _segments[i].load(std::memory_order_relaxed);
      if (!buckets)
        break;
      auto alloc = BucketAlloc(_alloc);
      BucketAllocTraits::deallocate(alloc, buckets, getSegmentSize(i));
    }

    for (full_iterator i = _list._head, e; i != e;) {
      auto prev = i++;
      if (prev->_key & size_type(1)) {
        auto alloc = NodeAlloc(_alloc);
        NodeAllocTraits::destroy(alloc, static_cast<Node *>(*prev));
        NodeAllocTraits::deallocate(alloc, static_cast<Node *>(*prev), 1);
      } else {
        auto alloc = NodeBaseAlloc(_alloc);
        NodeBaseAllocTraits::destroy(alloc, *prev);
        NodeBaseAllocTraits::deallocate(alloc, *prev, 1);
      }
    }
  }

  iterator begin() const { return ++iterator(_list._head); }

  iterator end() const { return iterator(); }

  std::pair<iterator, bool> insert(const value_type &val) {
    size_type key = _hasher(Traits::extractKey(val));
    auto orderKey = soRegular(key);
    auto alloc = NodeAlloc(_alloc);
    auto node = NodeAllocTraits::allocate(alloc, 1);
    NodeAllocTraits::construct(alloc, node, soRegular(key), val);
    auto bucketIndex = key % _size.load(std::memory_order_relaxed);

    if (!isBucketInitialized(bucketIndex))
      initalizeBucket(bucketIndex);

    auto bucket = getBucket(bucketIndex);

    full_iterator prev(bucket);
    full_iterator cur(bucket);
    full_iterator end;
    ++cur;
    while (true) {
      if (cur == end || cur->_key > orderKey) {
        // We've found where to add the node, try to insert.
        if (_list.insert(prev, node, cur) == node) {
          auto size = _size.load(std::memory_order_acquire);
          if (++_count / size >= maxLoadFactor)
            _size.compare_exchange_strong(size, size * 2);
          return std::make_pair(node, true);
        } else {
          // Failed to insert. Try again starting from the last known good loc.
          cur = prev;
          ++cur;
          continue;
        }
      } else if (cur->_key == orderKey && _equal(Traits::extractKey(*iterator(cur)), Traits::extractKey(val))) {
        // Value already exists.
        NodeAllocTraits::destroy(alloc, node);
        NodeAllocTraits::deallocate(alloc, node, 1);
        return std::make_pair(*cur, false);
      }
      prev = cur++;
    }
  }

  iterator find(const key_type &k) {
    size_type key = _hasher(k);
    auto orderKey = soRegular(key);
    auto bucketIndex = key % _size.load(std::memory_order_relaxed);

    if (!isBucketInitialized(bucketIndex))
      return end();

    auto bucket = getBucket(bucketIndex);

    full_iterator cur(bucket);
    full_iterator e;
    ++cur;
    while (true) {
      if (cur == e || cur->_key > orderKey)
        return end();
      else if (cur->_key == orderKey && _equal(Traits::extractKey(*iterator(cur)), k))
        return *cur;
      ++cur;
    }
  }

private:
  static size_type getSegmentIndex(size_type bucket) {
    return llvm::getMSBIndex(bucket);
  }

  static size_type getSegmentOffset(size_type seg) {
    // Segment 0 has offset 0.
    return (size_type(1) << seg) & ~size_type(1);
  }

  static size_type getSegmentSize(size_type seg) {
    // Segment 0 has size 2.
    return seg ? size_type(1) << seg : 2;
  }

  full_iterator getBucket(size_type bucket) {
    auto seg = getSegmentIndex(bucket);
    auto offset = getSegmentOffset(seg);
    return full_iterator(_segments[seg].load(std::memory_order_acquire)[bucket - offset].load(std::memory_order_acquire));
  }

  void setBucket(size_type bucket, full_iterator dummyNode) {
    auto seg = getSegmentIndex(bucket);
    auto segment = _segments[seg].load(std::memory_order_acquire);
    if (!segment) {
      // Allocate a new bucket segment.
      auto segSize = getSegmentSize(seg);
      auto alloc = BucketAlloc(_alloc);
      auto buckets = BucketAllocTraits::allocate(alloc, segSize);
      // HACK: This is not the proper way to initialize an array of
      // std::atomic<T*> to nullptr, but sadly the alternatives generate
      // horrible code.
      std::memset(buckets, 0, sizeof(typename BucketAlloc::value_type) *
                              getSegmentSize(seg));

      if (!_segments[seg].compare_exchange_strong(segment, buckets))
        BucketAllocTraits::deallocate(alloc, buckets, segSize);
      else
        segment = buckets;
    }
    segment[bucket - getSegmentOffset(seg)].store(dummyNode, std::memory_order_release);
  }

  /// \brief Get the parent bucket of index by removing the highest set bit.
  static size_type getParentBucket(size_type index) {
    return index & ~(1 << llvm::getMSBIndex(index));
  }

  static size_type soRegular(size_type key) {
    // Reverse and set bottom bit.
    return detail::reverseBits(key) | size_type(1);
  }

  static size_type soDummy(size_type key) {
    // Reverse and clear bottom bit.
    return detail::reverseBits(key) & ~size_type(1);
  }

  bool isBucketInitialized(size_type bucket) {
    auto seg = getSegmentIndex(bucket);
    auto segment = _segments[seg].load(std::memory_order_acquire);
    if (!segment)
      return false;
    return segment[bucket - getSegmentOffset(seg)].load(std::memory_order_acquire) != nullptr;
  }

  NodeBase *initalizeBucket(size_type bucket) {
    auto parentIndex = getParentBucket(bucket);
    if (!isBucketInitialized(parentIndex))
      initalizeBucket(parentIndex);
    auto parent = getBucket(parentIndex);

    // Create dummy node.
    auto alloc = NodeBaseAlloc(_alloc);
    auto dummyNode = NodeBaseAllocTraits::allocate(alloc, 1);
    NodeBaseAllocTraits::construct(alloc, dummyNode, soDummy(bucket));
    auto ins = _list.insert(parent, dummyNode);
    if (!ins.second) {
      // Another thread has already initalized this parent.
      NodeBaseAllocTraits::destroy(alloc, dummyNode);
      NodeBaseAllocTraits::deallocate(alloc, dummyNode, 1);
      dummyNode = ins.first;
    }
    // The dummyNode is still stored because the thread that added it to the
    // list may not have stored it in its bucket yet.
    setBucket(bucket, dummyNode);
    return dummyNode;
  }
};
} // end namespace lld

#endif
