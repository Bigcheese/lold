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
ThreadPool::ThreadPool(std::size_t threadCount) : _stop(false) {
  for (std::size_t i = 0; i < threadCount; ++i)
    _workers.push_back(std::thread([=]{work();}));
}

ThreadPool::~ThreadPool() {
  std::unique_lock<std::mutex> lock(_mutex);
  _stop = true;
  lock.unlock();
  _cond.notify_all();
  for (auto &worker : _workers)
    worker.join();
}

void ThreadPool::work() {
  while (true) {
    std::unique_lock<std::mutex> lock(_mutex);
    while (!_stop && _workQueue.empty())
      _cond.wait(lock);
    if (_stop && _workQueue.empty())
      return;
    auto task = _workQueue.front();
    _workQueue.pop();
    lock.unlock();
    task();
  }
}
} // end namespace lld
