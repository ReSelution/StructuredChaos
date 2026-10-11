#pragma once

#include <concepts>
#include <memory_resource>
#include <type_traits>
#include <utility>

namespace sc::detail {

  // A container of the std::pmr kind: all it owns is memory of its resource.
  template<typename T>
  concept PmrContainer = requires(const T &container) {
    typename T::value_type;
    typename T::allocator_type;
    { container.get_allocator().resource() } -> std::convertible_to<const std::pmr::memory_resource *>;
  } && std::same_as<typename T::allocator_type, std::pmr::polymorphic_allocator<typename T::value_type>>;

  // Whether an element of a pmr container owns nothing but memory of the
  // resource of that container. Such a container hands its resource on to its
  // elements, so nested pmr containers count as well.
  template<typename T>
  struct OwnsOnlyResourceMemory : std::is_trivially_destructible<T> {};

  template<typename T>
  inline constexpr bool ownsOnlyResourceMemory = OwnsOnlyResourceMemory<std::remove_cv_t<T>>::value;

  template<typename A, typename B>
  struct OwnsOnlyResourceMemory<std::pair<A, B>>
      : std::bool_constant<ownsOnlyResourceMemory<A> && ownsOnlyResourceMemory<B>> {};

  template<PmrContainer T>
  struct OwnsOnlyResourceMemory<T> : std::bool_constant<ownsOnlyResourceMemory<typename T::value_type>> {};

} // namespace sc::detail
