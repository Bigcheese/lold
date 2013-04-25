//===- lld/unittest/ParallelTest.cpp --------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief Parallel.h unit tests.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "lld/Core/Allocators.h"
#include "lld/Core/ConcurrentUnorderedMap.h"
#include "lld/Core/ConcurrentUnorderedSet.h"
#include "lld/Core/Parallel.h"

#include <random>

uint32_t array[1024 * 1024];

TEST(Parallel, sort) {
  std::mt19937 randEngine;
  std::uniform_int_distribution<uint32_t> dist;

  for (auto &i : array)
    i = dist(randEngine);

  lld::parallel_sort(std::begin(array), std::end(array));
  ASSERT_TRUE(std::is_sorted(std::begin(array), std::end(array)));
}

TEST(Parallel, SplitOrderedList) {
  struct Node {
    Node(std::size_t key) : _key(key), _next(nullptr) {}
    std::size_t _key;
    std::atomic<Node *> _next;
  };

  std::allocator<Node> alloc;
  lld::SplitOrderedList<Node> sol(alloc);

  lld::TaskGroup tg;
  for (unsigned i = 0; i < std::thread::hardware_concurrency(); ++i)
    tg.spawn([&] {
      std::mt19937 randEngine;
      std::uniform_int_distribution<uint32_t> dist;
      for (unsigned j = 0; j < 50; ++j) {
        auto node = new Node(dist(randEngine) | 3);
        if (!sol.insert(node).second)
          delete node;
      }
    });
  tg.sync();

  std::vector<std::size_t> vals;
  for (auto i = sol._head; i != nullptr; i = i->_next.load())
    vals.push_back(i->_key);
  EXPECT_EQ(51u, vals.size());
  EXPECT_TRUE(std::is_sorted(vals.begin(), vals.end()));
}

template <class T> bool is_unique(T start, T end) {
  std::set<typename std::iterator_traits<T>::value_type> s(start, end);
  return s.size() == std::size_t(std::distance(start, end));
}

TEST(Parallel, ConcurrentUnorderedSet) {
  lld::ConcurrentUnorderedSet<std::string> cus;

  lld::TaskGroup tg;
  for (unsigned i = 0; i < std::thread::hardware_concurrency(); ++i)
    tg.spawn([&] {
      std::mt19937 randEngine;
      std::uniform_int_distribution<uint32_t> dist;
      for (unsigned j = 0; j < 50; ++j) {
        cus.insert(std::to_string(j));
      }
    });
  tg.sync();

  EXPECT_TRUE(is_unique(cus.begin(), cus.end()));
  EXPECT_EQ(std::string("42"), *cus.find("42"));
}

TEST(Parallel, ConcurrentUnorderedMap) {
  lld::ConcurrentUnorderedMap<std::string, unsigned> cus;

  lld::TaskGroup tg;
  for (unsigned i = 0; i < std::thread::hardware_concurrency(); ++i)
    tg.spawn([&] {
      std::mt19937 randEngine;
      std::uniform_int_distribution<uint32_t> dist;
      for (unsigned j = 0; j < 50; ++j) {
        cus.insert(make_pair(std::to_string(j), j));
      }
    });
  tg.sync();

  EXPECT_TRUE(is_unique(cus.begin(), cus.end()));
  EXPECT_EQ(42u, cus.find("42")->second);
}

TEST(Parallel, RegionAllocatorSimple) {
  lld::RegionAllocator<int> ra;
  typedef std::allocator_traits<lld::RegionAllocator<int>> traits;
  for (unsigned i = 0; i < 1025; ++i) {
    auto p = traits::allocate(ra, 1);
    traits::construct(ra, p);
    traits::destroy(ra, p);
    traits::deallocate(ra, p, 1);
  }

  EXPECT_EQ(0u, ra.bytesAllocated());

  std::vector<int *> allocs;
  for (unsigned i = 0; i < 1025; ++i) {
    auto p = traits::allocate(ra, 1);
    traits::construct(ra, p);
    allocs.push_back(p);
  }

  for (unsigned i = 0; i < 1025; ++i) {
    auto p = allocs.back();
    traits::destroy(ra, p);
    traits::deallocate(ra, p, 1);
    allocs.pop_back();
  }
  EXPECT_EQ(0u, ra.bytesAllocated());
}

TEST(Parallel, RegionAllocator) {
  lld::RegionAllocator<int> ra;
  {
    std::set<int, std::less<int>, lld::RegionAllocator<int>> s(ra);
    s.insert(5);
    s.erase(5);
  }
  {
    // MSVC doesn't use std::allocator_traits in the implementation of
    // std::vector.
#ifndef _MSC_VER
    std::vector<char, lld::RegionAllocator<char>> v(ra);
    v.reserve(42);
#endif
  }
  // The standard makes no guarantee about what order std::set allocates and
  // deallocates in.
}

TEST(Parallel, ConcurrentRegionAllocator) {
  lld::ConcurrentRegionAllocator<int> ra;
  lld::ConcurrentUnorderedMap<int, int, std::hash<int>, std::equal_to<int>, lld::ConcurrentRegionAllocator<std::pair<int, int>>> cm(ra);

  lld::TaskGroup tg;
  for (unsigned i = 0; i < std::thread::hardware_concurrency(); ++i)
    tg.spawn([&] {
      std::mt19937 randEngine;
      std::uniform_int_distribution<uint32_t> dist;
      for (unsigned j = 0; j < 50; ++j) {
        cm.insert(std::make_pair(j, j));
      }
    });
  tg.sync();

  EXPECT_TRUE(is_unique(cm.begin(), cm.end()));
  EXPECT_EQ(42, cm.find(42)->second);

  // The standard makes no guarantee about what order std::set allocates and
  // deallocates in.
}
