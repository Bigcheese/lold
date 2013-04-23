//===- lld/Core/Parallel.h - Parallel utilities ---------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_CORE_PARALLEL_H
#define LLD_CORE_PARALLEL_H

#include "lld/Core/Instrumentation.h"
#include "lld/Core/LLVM.h"
#include "lld/Core/range.h"

#include "llvm/Support/MathExtras.h"

#ifdef _MSC_VER
// Exceptions are disabled so this isn't defined, but concrt assumes it is.
namespace {
void *__uncaught_exception() { return nullptr; }
}
#endif

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <stack>

#ifdef _MSC_VER
#include <concrt.h>
#include <ppl.h>
#endif

namespace lld {
/// \brief Allows one or more threads to wait on a potentially unknown number of
///   events.
///
/// A latch starts at \p count. inc() increments this, and dec() decrements it.
/// All calls to sync() will block while the count is not 0.
///
/// Calling dec() on a Latch with a count of 0 has undefined behaivor.
class Latch {
  uint32_t _count;
  mutable std::mutex _condMut;
  mutable std::condition_variable _cond;

public:
  explicit Latch(uint32_t count = 0) : _count(count) {}
  ~Latch() { sync(); }

  void inc() {
    std::unique_lock<std::mutex> lock(_condMut);
    ++_count;
  }

  void dec() {
    std::unique_lock<std::mutex> lock(_condMut);
    if (--_count == 0)
      _cond.notify_all();
  }

  void sync() const {
    std::unique_lock<std::mutex> lock(_condMut);
    _cond.wait(lock, [&] {
      return _count == 0;
    });
  }
};

/// \brief An abstract class that takes closures and runs them asynchronously.
class Executor {
public:
  virtual ~Executor() {}
  virtual void add(std::function<void()> func) = 0;
};

namespace detail {
  inline unsigned &getDefaultExecutorMaxConcurrency() {
    static unsigned maxCur = std::thread::hardware_concurrency();
    return maxCur;
  }
}

inline void setDefaultExecutorMaxConcurrency(unsigned maxCur) {
  detail::getDefaultExecutorMaxConcurrency() = maxCur;
}

inline unsigned getDefaultExecutorMaxConcurrency() {
  return detail::getDefaultExecutorMaxConcurrency();
}

/// \brief An implementation of an Executor that runs closures on a thread pool
///   in filo order.
class ThreadPoolExecutor : public Executor {
public:
  explicit ThreadPoolExecutor(unsigned threadCount =
                              getDefaultExecutorMaxConcurrency())
      : _stop(false), _done(threadCount) {
    // Spawn all but one of the threads in another thread as spawning threads
    // can take a while.
    std::thread([&, threadCount] {
      for (std::size_t i = 1; i < threadCount; ++i) {
        std::thread([=] {
          work();
        }).detach();
      }
      work();
    }).detach();
  }

  ~ThreadPoolExecutor() {
    std::unique_lock<std::mutex> lock(_mutex);
    _stop = true;
    lock.unlock();
    _cond.notify_all();
    // Wait for ~Latch.
  }

  virtual void add(std::function<void()> f) {
    std::unique_lock<std::mutex> lock(_mutex);
    _workStack.push(f);
    lock.unlock();
    _cond.notify_one();
  }

private:
  void work() {
    while (true) {
      std::unique_lock<std::mutex> lock(_mutex);
      _cond.wait(lock, [&] {
        return _stop || !_workStack.empty();
      });
      if (_stop)
        break;
      auto task = _workStack.top();
      _workStack.pop();
      lock.unlock();
      task();
    }
    _done.dec();
  }

  std::atomic<bool> _stop;
  std::stack<std::function<void()>> _workStack;
  std::mutex _mutex;
  std::condition_variable _cond;
  Latch _done;
};

#ifdef _MSC_VER
/// \brief An Executor that runs tasks via ConcRT.
class ConcRTExecutor : public Executor {
  struct Taskish {
    Taskish(std::function<void()> task) : _task(task) {}

    std::function<void()> _task;

    static void run(void *p) {
      Taskish *self = static_cast<Taskish *>(p);
      self->_task();
      concurrency::Free(self);
    }
  };

public:
  virtual void add(std::function<void()> func) {
    Concurrency::CurrentScheduler::ScheduleTask(Taskish::run,
        new (concurrency::Alloc(sizeof(Taskish))) Taskish(func));
  }
};

inline Executor *getDefaultExecutor() {
  static ConcRTExecutor exec;
  return &exec;
}
#else
inline Executor *getDefaultExecutor() {
  static ThreadPoolExecutor exec;
  return &exec;
}
#endif

/// \brief Allows launching a number of tasks and waiting for them to finish
///   either explicitly via sync() or implicitly on destruction.
class TaskGroup {
  Latch _latch;

public:
  void spawn(std::function<void()> f) {
    _latch.inc();
    getDefaultExecutor()->add([&, f] {
      f();
      _latch.dec();
    });
  }

  void sync() const { _latch.sync(); }
};

#ifdef _MSC_VER
// Use ppl parallel_sort on Windows.
template <class RandomAccessIterator, class Comp>
void parallel_sort(
    RandomAccessIterator start, RandomAccessIterator end,
    const Comp &comp = std::less<
        typename std::iterator_traits<RandomAccessIterator>::value_type>()) {
  concurrency::parallel_sort(start, end, comp);
}
#else
namespace detail {
const ptrdiff_t minParallelSize = 1024;

/// \brief Inclusive median.
template <class RandomAccessIterator, class Comp>
RandomAccessIterator medianOf3(RandomAccessIterator start,
                               RandomAccessIterator end, const Comp &comp) {
  RandomAccessIterator mid = start + (std::distance(start, end) / 2);
  return comp(*start, *(end - 1))
         ? (comp(*mid, *(end - 1)) ? (comp(*start, *mid) ? mid : start)
                                   : end - 1)
         : (comp(*mid, *start) ? (comp(*(end - 1), *mid) ? mid : end - 1)
                               : start);
}

template <class RandomAccessIterator, class Comp>
void parallel_quick_sort(RandomAccessIterator start, RandomAccessIterator end,
                         const Comp &comp, TaskGroup &tg, size_t depth) {
  // Do a sequential sort for small inputs.
  if (std::distance(start, end) < detail::minParallelSize || depth == 0) {
    std::sort(start, end, comp);
    return;
  }

  // Partition.
  auto pivot = medianOf3(start, end, comp);
  // Move pivot to end.
  std::swap(*(end - 1), *pivot);
  pivot = std::partition(start, end - 1, [end](decltype(*start) v) {
    return v < *(end - 1);
  });
  // Move pivot to middle of partition.
  std::swap(*pivot, *(end - 1));

  // Recurse.
  tg.spawn([=, &tg] {
    parallel_quick_sort(start, pivot, comp, tg, depth - 1);
  });
  parallel_quick_sort(pivot + 1, end, comp, tg, depth - 1);
}
}

template <class RandomAccessIterator, class Comp>
void parallel_sort(
    RandomAccessIterator start, RandomAccessIterator end,
    const Comp &comp = std::less<
        typename std::iterator_traits<RandomAccessIterator>::value_type>()) {
  TaskGroup tg;
  detail::parallel_quick_sort(start, end, comp, tg,
                              llvm::Log2_64(std::distance(start, end)) + 1);
}
#endif

template <class T> void parallel_sort(T *start, T *end) {
  parallel_sort(start, end, std::less<T>());
}

#ifdef _MSC_VER
// Use ppl parallel_for_each on Windows.
template <class Iterator, class Func>
void parallel_for_each(Iterator begin, Iterator end, Func func) {
  concurrency::parallel_for_each(begin, end, func);
}
#else
template <class Iterator, class Func>
void parallel_for_each(Iterator begin, Iterator end, Func func) {
  // TODO: Make this parallel.
  std::for_each(begin, end, func);
}
#endif

#ifdef _MSC_VER
#define LLVM_THREAD_LOCAL __declspec(thread)
#else
#define LLVM_THREAD_LOCAL __thread
#endif

struct RegionSlab {
  RegionSlab(std::size_t size) : _size(size), _next(nullptr) {}

  /// \brief size including the RegionSlab size.
  std::size_t _size;
  RegionSlab *_next;
};

template <class T, class Alloc = std::allocator<char>>
class RegionAllocator {
public:
  typedef T value_type;
  typedef T *pointer;
  typedef Alloc allocator_type;
  typedef std::allocator_traits<allocator_type> alloc_traits;

  RegionAllocator(std::size_t slabSize = 4096u, const allocator_type &a = allocator_type())
      : _owner(true), _slabs(nullptr), _slabSize(slabSize), _head(nullptr), _slabAlloc(a) {}

  template <class U>
  RegionAllocator(const RegionAllocator<U, Alloc> &other)
      : _owner(false), _slabs(other._slabs), _slabSize(other._slabSize), _head(other._head),
        _slabAlloc(std::allocator_traits<Alloc>::select_on_container_copy_construction(other._slabAlloc)) {}

  ~RegionAllocator() {
    if (!_owner)
      return;
    for (auto slab = _slabs; slab; ) {
      auto nextSlab = slab->_next;
      auto size = slab->_size;
      slab->~RegionSlab();
      alloc_traits::deallocate(_slabAlloc, reinterpret_cast<char *>(slab), size);
      slab = nextSlab;
    }
  }

  pointer allocate(std::size_t num) {
    if (!_slabs)
      insertSlab(allocateSlab(_slabSize));

    std::size_t allocSize = num * sizeof(value_type);
    std::size_t slabRemaining = _slabSize - std::distance(static_cast<char *>(_slabs), _head);
    void *alignedHead = std::align(llvm::alignOf<value_type>(), allocSize, alignedHead, slabRemaining);

    if (alignedHead) {
      // Allocation succeeded.
      __msan_allocated_memory(alignedHead, allocSize);
      _head = static_cast<char *>(alignedHead) + allocSize;
      return static_cast<pointer>(alignedHead);
    }

    // Check if it's too large for a single slab.
    std::size_t paddedSize = llvm::RoundUpToAlignment(sizeof(RegionSlab), llvm::alignOf<value_type>()) + allocSize;
    if (paddedSize > _slabSize) {
      auto newSlab = allocateSlab(paddedSize);
      // Insert the new slab behind the current slab.
      newSlab->_next = _slabs->_next;
      _slabs->_next = newSlab;
      alignedHead = std::align(llvm::alignOf<value_type>(), allocSize, alignedHead, paddedSize);
      assert(alignedHead && "Unable to allocate memory!");
      __msan_allocated_memory(alignedHead, allocSize);
      return static_cast<pointer>(alignedHead);
    }

    // Start a new slab and try again.
    insertSlab(allocateSlab(_slabSize));
    slabRemaining = _slabSize - std::distance(static_cast<char *>(_slabs), _head);
    alignedHead = std::align(llvm::alignOf<value_type>(), allocSize, alignedHead, slabRemaining);
    assert(alignedHead && "Unable to allocate memory!");
    __msan_allocated_memory(alignedHead, allocSize);
    _head = static_cast<char *>(alignedHead) + allocSize;
    return static_cast<pointer>(alignedHead);
  }

private:
  RegionSlab *allocateSlab(std::size_t size) {
    auto slab = alloc_traits::allocate(_slabAlloc, size);
    return new (slab) RegionSlab(size);
  }

  void insertSlab(RegionSlab *slab) {
    slab->_next = _slabs;
    _slabs = slab;
    _head = slab + sizeof(RegionSlab);
  }

  /// \brief True if this instance owns the RegionSlab chain.
  bool _owner;
  RegionSlab *_slabs;
  std::size_t _slabSize;
  typename alloc_traits::pointer _head;
  allocator_type _slabAlloc;
};

/// \brief A lifo allocator whith a separate region per instance per thread.
class ConcurrentRegionAllocator {
public:
  static LLVM_THREAD_LOCAL void *v;
};
} // end namespace lld

#endif
