#pragma once
#include <cstring>
#include <concepts>
#include <type_traits>


namespace SC {

  template<typename T>
  concept IsContiguousResource = requires(T t)
  {
    { t.data() };
    { t.size() } -> std::convertible_to<std::size_t>;
    typename T::value_type;
  };


  template<IsContiguousResource ResourceType>
  struct ChaosResource {
    static constexpr bool is_chaos_resource = true;
    std::pmr::memory_resource* pool = PoolAnchor::current;
    ResourceType view{};

    template<typename T>
      requires requires(T t) { { t.data() }; { t.size() }; }
    ChaosResource(const T &initialVal) {
      *this = initialVal;
    }
    constexpr ChaosResource() noexcept : pool(nullptr) {}
    ChaosResource(ChaosResource& other) = delete;
    ChaosResource(ChaosResource&& other) noexcept : view(other.view) {
      other.view = {};
    }

    // ~ChaosResource() {
    //   clear();
    // }
    //
    void clear() {
      if (view.data()) {
        size_t oldByteSize = view.size() * sizeof(typename ResourceType::value_type);
        pool->deallocate(const_cast<void*>(static_cast<const void*>(view.data())), oldByteSize);
      }
    }
    template<typename T>
      requires requires(T t) { { t.data() }; { t.size() }; }
    ChaosResource &operator=(const T &newVal) {
      assert(pool != nullptr);

      if (view.data() == newVal.data()) {
        return *this;
      }
      if (view.data()) {
        size_t oldByteSize = view.size() * sizeof(typename ResourceType::value_type);
        pool->deallocate(const_cast<void*>(static_cast<const void*>(view.data())), oldByteSize);
      }

      if (newVal.empty()) {
        view = {};
        return *this;
      }

      const size_t byteSize = newVal.size() * sizeof(typename ResourceType::value_type);
      void *buf = pool->allocate(byteSize);
      std::memcpy(buf, newVal.data(), byteSize);

      view = ResourceType{static_cast<typename ResourceType::value_type *>(buf), newVal.size()};

      return *this;
    }

    operator ResourceType() const { return view; }

    const ResourceType *operator->() const { return &view; }

    auto begin() const { return view.begin(); }
    auto end() const { return view.end(); }

    auto begin() requires (!std::is_const_v<typename ResourceType::value_type>) {
      return view.begin();
    }

    auto end() requires (!std::is_const_v<typename ResourceType::value_type>) {
      return view.end();
    }

    decltype(auto) operator[](std::size_t index) const { return view[index]; }

    decltype(auto) operator[](std::size_t index)
      requires (!std::is_const_v<typename ResourceType::value_type>) {
      return view[index];
    }

    std::size_t size() const { return view.size(); }
    bool empty() const { return view.empty(); }
  };

  template<typename T>
  concept IsChaosResource = requires {
    { T::is_chaos_resource } -> std::convertible_to<bool>;
  } && T::is_chaos_resource;
} // SC
