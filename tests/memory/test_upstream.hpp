#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory_resource>
#include <mutex>
#include <new>

// Upstream for the tests. It records every block it hands out, checks that
// each one comes back exactly as it was requested, and never aligns a block
// better than it was asked to.
class TrackingUpstream : public std::pmr::memory_resource {
  struct Info {
    void *raw;
    size_t size;
    size_t alignment;
  };

  mutable std::mutex m_mutex;
  std::map<void *, Info> m_live;

public:
  // A resource that keeps asking for blocks would otherwise never return.
  size_t maxAllocations = 64;
  // Stands in for running out of memory.
  size_t maxBlockSize = size_t{1} << 30;

  size_t allocations = 0;
  size_t deallocations = 0;
  size_t mismatches = 0;
  size_t lastSize = 0;
  size_t lastAlignment = 0;

  ~TrackingUpstream() override {
    for (auto &[ptr, info] : m_live) {
      ::operator delete(info.raw, std::align_val_t(info.alignment * 2));
    }
  }

  [[nodiscard]] size_t liveBlocks() const {
    std::lock_guard lock(m_mutex);
    return m_live.size();
  }

  // Whether ptr lies inside one of the blocks that are currently handed out.
  [[nodiscard]] bool owns(const void *ptr) const {
    std::lock_guard lock(m_mutex);
    auto addr = reinterpret_cast<uintptr_t>(ptr);
    for (const auto &[start, info] : m_live) {
      auto begin = reinterpret_cast<uintptr_t>(start);
      if (addr >= begin && addr < begin + info.size) {
        return true;
      }
    }
    return false;
  }

protected:
  void *do_allocate(size_t bytes, size_t alignment) override {
    std::lock_guard lock(m_mutex);
    if (allocations >= maxAllocations || bytes > maxBlockSize) {
      throw std::bad_alloc();
    }
    ++allocations;
    lastSize = bytes;
    lastAlignment = alignment;

    // Offset by one alignment step inside a block aligned twice as strictly:
    // the result has the requested alignment, but never a better one.
    void *raw = ::operator new(bytes + alignment * 2,
                               std::align_val_t(alignment * 2));
    void *ptr = static_cast<std::byte *>(raw) + alignment;
    m_live.emplace(ptr, Info{raw, bytes, alignment});
    return ptr;
  }

  void do_deallocate(void *ptr, size_t bytes, size_t alignment) override {
    std::lock_guard lock(m_mutex);
    ++deallocations;
    auto it = m_live.find(ptr);
    if (it == m_live.end()) {
      ++mismatches;
      return;
    }
    if (it->second.size != bytes || it->second.alignment != alignment) {
      ++mismatches;
    }
    ::operator delete(it->second.raw,
                      std::align_val_t(it->second.alignment * 2));
    m_live.erase(it);
  }

  [[nodiscard]] bool
  do_is_equal(const memory_resource &other) const noexcept override {
    return this == &other;
  }
};
