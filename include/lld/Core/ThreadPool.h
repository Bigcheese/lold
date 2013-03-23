//===- lld/Core/ThreadPool.h - Thread Pool --------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_CORE_THREAD_POOL_H
#define LLD_CORE_THREAD_POOL_H

#ifdef WIN32
namespace {
void *__uncaught_exception() { return nullptr; }
}
#endif

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <queue>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace lld {
class ThreadPool {
public:
  ThreadPool(std::size_t threadCount = std::thread::hardware_concurrency());
  ~ThreadPool();

  void enqueue(std::function<void()> f) {
    std::unique_lock<std::mutex> lock(_mutex);
    assert(!_stop && "Cannot add task to stopped thread pool!");
    _workQueue.push(f);
    lock.unlock();
    _cond.notify_one();
  }

  void sync();

private:
  void work();

  std::vector<std::thread> _workers;
  std::queue<std::function<void()>> _workQueue;
  std::mutex _mutex;
  std::condition_variable _cond;
  std::atomic<bool> _stop;
  std::atomic<bool> _initalized;
};
} // end namespace lld

#endif
