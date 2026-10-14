#pragma once

#ifndef SC_FORCE_INLINE
#ifdef _MSC_VER
#define SC_FORCE_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define SC_FORCE_INLINE inline __attribute__((always_inline))
#else
#define SC_FORCE_INLINE inline
#endif
#endif
