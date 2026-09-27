//
// Created by oleub on 09.04.26.
//

#pragma once

#include <memory_resource>
namespace sc::ecs {
struct PoolAnchor {
  static inline thread_local std::pmr::memory_resource *current = nullptr;
};
} // namespace sc::ecs
