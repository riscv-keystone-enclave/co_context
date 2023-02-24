#pragma once

#include <memory>

#ifdef __cpp_lib_assume_aligned
#define CO_CONTEXT_ASSUME_ALIGNED(align) std::assume_aligned<align>
#else
#define CO_CONTEXT_ASSUME_ALIGNED(...)
#endif

#if defined(__x86_64__) || defined(__i386__)
#if defined(__clang__) || defined(__GNUC__) || defined(__GNUG__)
#define CO_CONTEXT_PAUSE __builtin_ia32_pause()
#elif defined(_MSC_VER)
#define CO_CONTEXT_PAUSE _mm_pause()
#else
#warning CO_CONTEXT_PAUSE has an empty definition for unknown compiler
#define CO_CONTEXT_PAUSE
#endif
#elif defined(__aarch64__)
#define CO_CONTEXT_PAUSE asm volatile("yield")
#else
#warning CO_CONTEXT_PAUSE has an empty definition for unknown architecture
#define CO_CONTEXT_PAUSE
#endif

#if (defined(__GNUC__) || defined(__GNUG__)) \
    && __has_include(<sys/single_threaded.h>)
#define CO_CONTEXT_IS_SINGLE_THREADED (__gnu_cxx::__is_single_threaded())
#else
// TODO find out ways to judge this
#define CO_CONTEXT_IS_SINGLE_THREADED false
#endif
