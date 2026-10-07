#pragma once
#include <cassert>
#include <concepts>
#include <cstring>
#include <type_traits>

#include <mimalloc.h>

#include "anchor.hpp"
namespace sc::ecs {

  template <typename T>
  concept IsContiguousResource = requires(T t) {
    { t.data() };
    { t.size() } -> std::convertible_to<std::size_t>;
    typename T::value_type;
  };

  template <IsContiguousResource ResourceType> struct Resource {
    static constexpr bool is_resource = true;
    // Only needed to allocate; a buffer is freed without it.
    mi_heap_t *heap = HeapAnchor::current;
    ResourceType view{};

    template <typename T>
      requires requires(T t) {
        { t.data() };
        { t.size() };
      }
    Resource(const T &initialVal) {
      *this = initialVal;
    }
    constexpr Resource() noexcept : heap(nullptr) {}
    Resource &operator=(const Resource &) = delete;
    Resource &operator=(Resource &&) = delete;
    Resource(Resource &other) = delete;
    Resource(Resource &&other) noexcept : heap(other.heap), view(other.view) { other.view = {}; }

    void clear() {
      if (view.data()) {
        mi_free(const_cast<void *>(static_cast<const void *>(view.data())));
      }
    }
    template <typename T>
      requires requires(T t) {
        { t.data() };
        { t.size() };
      }
    Resource &operator=(const T &newVal) {
      assert(heap != nullptr);

      if (view.data() == newVal.data()) {
        return *this;
      }
      if (view.data()) {
        mi_free(const_cast<void *>(static_cast<const void *>(view.data())));
      }

      if (newVal.empty()) {
        view = {};
        return *this;
      }

      const size_t byteSize = newVal.size() * sizeof(typename ResourceType::value_type);
      void *buf = mi_heap_malloc_aligned(heap, byteSize, alignof(typename ResourceType::value_type));
      std::memcpy(buf, newVal.data(), byteSize);

      view = ResourceType{static_cast<ResourceType::value_type *>(buf), newVal.size()};

      return *this;
    }

    operator ResourceType() const { return view; }

    const ResourceType *operator->() const { return &view; }

    [[nodiscard]] auto begin() const { return view.begin(); }
    [[nodiscard]] auto end() const { return view.end(); }

    auto begin()
      requires(!std::is_const_v<typename ResourceType::value_type>)
    {
      return view.begin();
    }

    auto end()
      requires(!std::is_const_v<typename ResourceType::value_type>)
    {
      return view.end();
    }

    decltype(auto) operator[](std::size_t index) const { return view[index]; }

    decltype(auto) operator[](std::size_t index)
      requires(!std::is_const_v<typename ResourceType::value_type>)
    {
      return view[index];
    }

    [[nodiscard]] std::size_t size() const { return view.size(); }
    [[nodiscard]] bool empty() const { return view.empty(); }
  };

  template <typename T>
  concept IsResource = requires {
    { T::is_resource } -> std::convertible_to<bool>;
  } && T::is_resource;
} // namespace sc::ecs
