//===- lib/Core/ThreadPool.cpp - Thread Pool ------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lld/Core/ThreadPool.h"

namespace lld {
ThreadPool::ThreadPool(std::size_t threadCount)
    : _workers(threadCount), _stop(false), _initalized(false) {
  // Spawn all but one of the threads in another thread as spawning threads can
  // take a while.
  _workers[0] = std::thread([&, threadCount] {
    for (std::size_t i = 1; i < threadCount; ++i) {
      std::unique_lock<std::mutex> lock(_mutex);
      if (_stop && _workQueue.empty())
        break;
      lock.unlock();
      _workers[i] = std::thread([=] {work();});
    }
    _initalized = true;
    work();
  });
}

ThreadPool::~ThreadPool() {
  if (!_stop)
    sync();
}

void ThreadPool::sync() {
  _stop = true;
  _cond.notify_all();
  while (!_initalized)
    std::this_thread::yield();
  for (auto &worker : _workers)
    if (worker.joinable())
      worker.join();
}

void ThreadPool::work() {
  while (true) {
    std::unique_lock<std::mutex> lock(_mutex);
    _cond.wait(lock, [&] {return _stop || !_workQueue.empty();});
    if (_stop && _workQueue.empty())
      return;
    auto task = _workQueue.front();
    _workQueue.pop();
    lock.unlock();
    task();
  }
}
} // end namespace lld
