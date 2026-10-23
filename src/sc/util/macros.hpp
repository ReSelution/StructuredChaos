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

// Never inlined: the rare path out of a hot function, so that the hot one
// stays small enough to be inlined itself. clang-cl takes the GNU form too.
#ifndef SC_NOINLINE
#if defined(__GNUC__) || defined(__clang__)
#define SC_NOINLINE __attribute__((noinline))
#elifdef _MSC_VER
#define SC_NOINLINE __declspec(noinline)
#else
#define SC_NOINLINE
#endif
#endif

// Rarely called: GCC and Clang optimize it for size, put it in .text.unlikely
// and take the paths that lead to a call to it as unlikely. MSVC has nothing
// of the kind.
#ifndef SC_COLD
#if defined(__GNUC__) || defined(__clang__)
#define SC_COLD __attribute__((cold))
#else
#define SC_COLD
#endif
#endif

// Called often: GCC and Clang put it in .text.hot, next to the other hot
// functions. It does not get it inlined: a callee too large for the
// threshold stayed a call with the callee, the callers or both marked (GCC
// 16 and Clang 22, -O2). MSVC has nothing of the kind.
#ifndef SC_HOT
#if defined(__GNUC__) || defined(__clang__)
#define SC_HOT __attribute__((hot))
#else
#define SC_HOT
#endif
#endif
