module;

#include <memory_resource>
export module sc.ecs:anchor;

namespace sc::ecs {
  export struct PoolAnchor {
    static inline thread_local std::pmr::memory_resource *current = nullptr;
  };
} // namespace sc::ecs
