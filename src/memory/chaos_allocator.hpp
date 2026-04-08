//
// Created by oleub on 31.03.26.
//

#pragma once
#include <cstddef>

namespace SC {
  class ChaosAllocator {
    public:

    void * allocate(size_t size, size_t alignment);
    void deallocate(void * ptr);

    private:



  };
} // SC