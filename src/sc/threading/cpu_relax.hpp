#pragma once

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif

namespace sc {

  // Tells the CPU that this thread is spinning, which saves power and leaves
  // the core to its hyper-thread sibling. Does nothing where there is no such
  // instruction.
  inline void cpuRelax() noexcept {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    _mm_pause();
#elif defined(__aarch64__) || defined(__ARM_ARCH)
#ifdef _MSC_VER
    __yield();
#else
    asm volatile("yield" ::: "memory");
#endif
#endif
  }

} // namespace sc
