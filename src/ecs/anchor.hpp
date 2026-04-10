//
// Created by oleub on 09.04.26.
//

#pragma once

namespace std::pmr {
  class memory_resource;
}

namespace SC {
  struct PoolAnchor {
    static inline thread_local std::pmr::memory_resource *current = nullptr;
  };
}
