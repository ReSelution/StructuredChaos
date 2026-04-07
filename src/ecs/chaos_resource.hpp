#pragma once
#include <cstring>
#include <concepts>
#include <type_traits>
#include "mimalloc.h"

namespace SC {
#define CHAOS_MANAGED(...) \
static void _chaos_cleanup(entt::registry& reg, entt::entity e) { \
auto& comp = reg.get<decltype(*this)>(e); \
auto cleanup_one = [&](auto& field) { \
if constexpr (requires { field.view; }) { \
if (field.view.data()) { \
mi_free(const_cast<void*>(static_cast<const void*>(field.view.data()))); \
field.view = {}; \
} \
} \
}; \
(cleanup_one(comp.__VA_ARGS__), ...); \
}

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
    ResourceType view{};


    template<typename T>
      requires requires(T t) { { t.data() }; { t.size() }; }
    ChaosResource(const T &initialVal) {
      *this = initialVal;
    }

    ChaosResource(ChaosResource& other) = delete;
    ChaosResource(ChaosResource&& other) noexcept : view(other.view) {
      other.view = {};
    }

    template<typename T>
      requires requires(T t) { { t.data() }; { t.size() }; }
    ChaosResource &operator=(const T &newVal) {
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
      void *buf = mi_malloc(byteSize);
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
} // SC
