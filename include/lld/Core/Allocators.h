//===- lld/Core/Allocators.h - Custom allocaotrs --------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_CORE_ALLOCATORS_H
#define LLD_CORE_ALLOCATORS_H

extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);

#include "lld/Core/ConcurrentUnorderedMap.h"

namespace lld {
struct RegionSlab {
  RegionSlab(std::size_t size, std::size_t alignment)
      : _size(size),
        _head(reinterpret_cast<char *>(this) + llvm::RoundUpToAlignment(alignment, sizeof(RegionSlab))),
        _next(nullptr) {}

  std::size_t spaceRemaining() const {
    return _size - (_head - reinterpret_cast<const char *>(this));
  }

  bool isEmpty(std::size_t alignment) const {
    return _head == reinterpret_cast<const char *>(this) + llvm::RoundUpToAlignment(alignment, sizeof(RegionSlab));
  }

  /// \brief size including sizeof(RegionSlab). Used to pass the correct value
  ///   to A.deallocate().
  std::size_t _size;

  /// \brief Current allocation location for this slab. This is stored per slab
  ///   to allow lifo deallocation.
  char *_head;

  /// \brief Next slab in the slab chain.
  RegionSlab *_next;
};

/// \brief Get the 0 based index of the most significant bit set in val.
template <std::size_t val>
struct MSBIndex;

template <std::size_t val>
struct MSBIndex {
  static const std::size_t value = MSBIndex<(val >> 1)>::value + 1;
};

template <>
struct MSBIndex<0> {
  static const std::size_t value = -1;
};

template <class Alloc>
struct RegionAllocatorState {
  typedef Alloc allocator_type;
  typedef std::allocator_traits<allocator_type> alloc_traits;

  RegionAllocatorState(std::size_t slabSize, const allocator_type &a)
     : _largeSlab(nullptr), _slabSize(slabSize), _slabAlloc(a), _bytesAllocated(0) {
    std::uninitialized_fill(_curSlab.begin(), _curSlab.end(), nullptr);
  }

  ~RegionAllocatorState() {
    for (auto slab : _curSlab)
      destroyChain(slab);
    destroyChain(_largeSlab);
  }

  void *allocate(std::size_t size, std::size_t alignment) {
    _bytesAllocated += size;

    if (alignment == 0)
      alignment = 1;

    if (alignment > _curSlab.size() || size > _slabSize - llvm::RoundUpToAlignment(sizeof(RegionSlab), alignment))
      return allocateLarge(size, alignment);

    auto &slab = _curSlab[llvm::getMSBIndex(alignment)];

    if (!slab)
      slab = allocateSlab(_slabSize, alignment);

  retry:
    std::size_t spaceRemaining = slab->spaceRemaining();
    if (spaceRemaining >= size) {
      // Allocation succeeded.
      void *ret = slab->_head;
      slab->_head += size;
      __msan_allocated_memory(ret, size);
      return ret;
    }

    // Allocate new slab and try again.
    auto newSlab = allocateSlab(_slabSize, alignment);
    newSlab->_next = slab;
    slab = newSlab;
    goto retry;
  }

  void deallocate(void *p, std::size_t size, std::size_t alignment) {
    if (alignment == 0)
      alignment = 1;

    if (alignment > _curSlab.size() ||
        size > _slabSize -
               llvm::RoundUpToAlignment(sizeof(RegionSlab), alignment)) {
      // It was allocated to _largeSlab.
      if (!_largeSlab || static_cast<char *>(p) + size != _largeSlab->_head)
        return; // Not the last allocation.
      auto toRemove = _largeSlab;
      _largeSlab = _largeSlab->_next;
      deallocateSlab(toRemove);
      _bytesAllocated -= size;
      return;
    }

    auto &slab = _curSlab[llvm::getMSBIndex(alignment)];

    if (!slab)
      return;

    // Check if the current slab is already empty. Delete it if so.
    if (slab->isEmpty(alignment)) {
      auto toRemove = slab;
      slab = slab->_next;
      deallocateSlab(toRemove);
    }

    if (static_cast<char *>(p) + size == slab->_head) {
      slab->_head = static_cast<char *>(p);
      _bytesAllocated -= size;
    }
  }

  std::size_t bytesAllocated() const {
    return _bytesAllocated;
  }

private:
  void *allocateLarge(std::size_t size, std::size_t alignment) {
    std::size_t paddedSize = llvm::RoundUpToAlignment(sizeof(RegionSlab), alignment) + size;
    auto newSlab = allocateSlab(paddedSize, alignment);
    newSlab->_next = _largeSlab;
    _largeSlab = newSlab;
    void *ret = newSlab->_head;
    newSlab->_head += size;
    __msan_allocated_memory(ret, size);
    return ret;
  }

  RegionSlab *allocateSlab(std::size_t size, std::size_t alignment) {
    auto slab = alloc_traits::allocate(_slabAlloc, size);
    return new (slab) RegionSlab(size, alignment);
  }

  void deallocateSlab(RegionSlab *slab) {
    auto slabSize = slab->_size;
    slab->~RegionSlab();
    alloc_traits::deallocate(_slabAlloc, reinterpret_cast<char *>(slab), slabSize);
  }

  void destroyChain(RegionSlab *slab) {
    if (!slab)
      return;

    RegionSlab *next;
    do {
      next = slab->_next;
      std::size_t size = slab->_size;
      slab->~RegionSlab();
      alloc_traits::deallocate(_slabAlloc, reinterpret_cast<char *>(slab), size);
      slab = next;
    } while (next);
  }

  /// \brief slab per alignment up to alignof(std::max_align_t).
  std::array<RegionSlab *, MSBIndex<llvm::AlignOf<std::max_align_t>::Alignment>::value + 1> _curSlab;
  RegionSlab *_largeSlab;
  std::size_t _slabSize;
  allocator_type _slabAlloc;
  std::size_t _bytesAllocated;
};

template <class T, class Alloc = std::allocator<char>>
class RegionAllocator {
public:
  typedef T value_type;
  typedef Alloc allocator_type;

  template <class OtherT, class OtherAlloc> friend class RegionAllocator;

  RegionAllocator(std::size_t slabSize = 4096u, const allocator_type &a = allocator_type())
      : _state(std::make_shared<RegionAllocatorState<Alloc>>(slabSize, a)) {}

  template <class U>
  RegionAllocator(const RegionAllocator<U, Alloc> &other)
      : _state(other._state) {}

  T *allocate(std::size_t num) {
    return (T *)_state->allocate(sizeof(T) * num, llvm::alignOf<T>());
  }

  void deallocate(T *p, std::size_t num) {
    _state->deallocate(p, sizeof(T) * num, llvm::alignOf<T>());
  }

  std::size_t bytesAllocated() const {
    return _state->bytesAllocated();
  }

  // MSVC does not properly implement std::allocator_traits::rebind_alloc, so we
  // have to provide this.
  template<class U> struct rebind {
    typedef RegionAllocator<U, Alloc> other;
  };

private:
  std::shared_ptr<RegionAllocatorState<Alloc>> _state;
};

template <class Alloc>
class ConcurrentRegionAllocatorState {
public:
  ConcurrentRegionAllocatorState(std::size_t slabSize, const Alloc &a)
      : _slabSize(slabSize), _alloc(a) {}

  void *allocate(std::size_t size, std::size_t alignment) {
    auto thread = GetCurrentThreadId();
    auto allocator = _regionMap.find(thread);
    if (allocator == _regionMap.end()) {
      allocator = _regionMap.insert(make_pair(thread, RegionAllocatorState<Alloc>(_slabSize, _alloc))).first;
    }

    return allocator->second.allocate(size, alignment);
  }

  void deallocate(void *p, std::size_t size, std::size_t alignment) {
    auto thread = GetCurrentThreadId();
    auto allocator = _regionMap.find(thread);
    if (allocator == _regionMap.end()) {
      return;
    }

    return allocator->second.deallocate(p, size, alignment);
  }

private:
  typedef ConcurrentUnorderedMap<DWORD,
                                 RegionAllocatorState<Alloc>> RegionMap;

  RegionMap _regionMap;
  std::size_t _slabSize;
  Alloc _alloc;
};

template <class T, class Alloc = std::allocator<char>>
/// \brief A lifo allocator whith a separate region per instance per thread.
class ConcurrentRegionAllocator {
public:
  typedef T value_type;
  typedef Alloc allocator_type;

  template <class OtherT, class OtherAlloc> friend class ConcurrentRegionAllocator;

  ConcurrentRegionAllocator(std::size_t slabSize = 4096u, const allocator_type &a = allocator_type())
      : _state(std::make_shared<ConcurrentRegionAllocatorState<Alloc>>(slabSize, a)) {}

  template <class U>
  ConcurrentRegionAllocator(const ConcurrentRegionAllocator<U, Alloc> &other)
      : _state(other._state) {}

  T *allocate(std::size_t num) {
    return (T *)_state->allocate(sizeof(T) * num, llvm::alignOf<T>());
  }

  void deallocate(T *p, std::size_t num) {
    _state->deallocate(p, sizeof(T) * num, llvm::alignOf<T>());
  }

  // MSVC does not properly implement std::allocator_traits::rebind_alloc, so we
  // have to provide this.
  template<class U> struct rebind {
    typedef ConcurrentRegionAllocator<U, Alloc> other;
  };

private:
  std::shared_ptr<ConcurrentRegionAllocatorState<Alloc>> _state;
};
} // end namespace lld

#endif
