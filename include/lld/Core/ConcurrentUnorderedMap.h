//===- ConcurrentUnorderedMap.h - A lock-free unordered map ---------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief Implements a Split Ordered List based lock-free unordered map.
///
/// The interface is modeled after the n3425 proposal:
/// http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2012/n3425.html
///
/// Paper:
/// http://dl.acm.org/citation.cfm?id=1147958
///
//===----------------------------------------------------------------------===//

#ifndef LLD_CORE_CONCURRENT_UNORDERED_MAP_H
#define LLD_CORE_CONCURRENT_UNORDERED_MAP_H

#include "lld/Core/ConcurrentUnorderedBase.h"

template <class Key,
          class Value,
          class Hash = std::hash<Key>,
          class Pred = std::equal_to<Key>,
          class Allocator = std::allocator<std::pair<const Key, Value>>>
struct ConcurrentUnorderedMapTraits {
  typedef Key key_type;
  typedef std::pair<const Key, Value> value_type;
  typedef Hash hasher;
  typedef Pred key_equal;
  typedef Allocator allocator_type;

  static const Key &extractKey(const value_type &val) {
    return val.first;
  }
};

namespace lld {
/// \brief A lock-free hash map.
template <class Key,
          class Value,
          class Hash = std::hash<Key>,
          class Pred = std::equal_to<Key>,
          class Allocator = std::allocator<std::pair<const Key, Value>>>
class ConcurrentUnorderedMap : public ConcurrentUnorderedBase<ConcurrentUnorderedMapTraits<Key, Value, Hash, Pred, Allocator>> {
  typedef ConcurrentUnorderedBase<ConcurrentUnorderedMapTraits<Key, Value, Hash, Pred, Allocator>> BaseType;

public:
  typedef typename BaseType::size_type size_type;
  typedef typename BaseType::hasher hasher;
  typedef typename BaseType::key_equal key_equal;
  typedef typename BaseType::allocator_type allocator_type;

  ConcurrentUnorderedMap(size_type n = 8, const hasher &h = hasher(),
                         const key_equal &ke = key_equal(),
                         const allocator_type &a = allocator_type())
      : BaseType(n, h, ke, a) {}

  ConcurrentUnorderedMap(const allocator_type &a)
      : BaseType(8, hasher(), key_equal(), a) {}
};
} // end namespace lld

#endif
