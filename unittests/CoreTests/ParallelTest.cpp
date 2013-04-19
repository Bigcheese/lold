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
}
