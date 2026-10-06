//
// Created by oleub on 09.04.26.
//

#pragma once

#include <mimalloc.h>
namespace sc::ecs {
// Heap that Resource fields constructed on this thread allocate from.
struct HeapAnchor {
  static inline thread_local mi_heap_t *current = nullptr;
};
} // namespace sc::ecs
