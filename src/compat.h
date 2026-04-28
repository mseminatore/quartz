//---------------------------------------------------------------------------
// Copyright 2025. All rights reserved.
//
// Compiler compatibility helpers — abstracts GCC/Clang vs MSVC differences.
//---------------------------------------------------------------------------
#ifndef RTOS_COMPAT_H
#define RTOS_COMPAT_H

#ifdef _MSC_VER
#   include <intrin.h>
#endif

// ---------------------------------------------------------------------------
// RTOS_WEAK — mark a function as a weak default that the user can override.
// On MSVC, weak symbols are not supported; the stubs are compiled as regular
// functions.  When building for MSVC, supply overrides via a separate
// compilation unit and linker flags (or rely on the defaults).
// ---------------------------------------------------------------------------

#if defined(__GNUC__) || defined(__clang__)
#   define RTOS_WEAK  __attribute__((weak))
#else
#   define RTOS_WEAK
#endif

// ---------------------------------------------------------------------------
// RTOS_USED — prevent the linker from discarding an unreferenced symbol.
// ---------------------------------------------------------------------------

#if defined(__GNUC__) || defined(__clang__)
#   define RTOS_USED  __attribute__((used))
#else
#   define RTOS_USED
#endif

// ---------------------------------------------------------------------------
// RTOS_NAKED — mark a function as having no compiler-generated prolog/epilog.
// ---------------------------------------------------------------------------

#if defined(__GNUC__) || defined(__clang__)
#   define RTOS_NAKED  __attribute__((naked))
#else
#   define RTOS_NAKED
#endif

// ---------------------------------------------------------------------------
// rtos_ctz — count trailing zeros (finds the index of the lowest set bit).
// Used by the scheduler to find the highest-priority ready queue in O(1).
// ---------------------------------------------------------------------------

static inline int rtos_ctz(unsigned int val)
{
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_ctz(val);
#elif defined(_MSC_VER)
    unsigned long idx;
    _BitScanForward(&idx, (unsigned long)val);
    return (int)idx;
#else
    // Portable fallback — de Bruijn sequence
    static const int debruijn32[32] = {
         0,  1, 28,  2, 29, 14, 24,  3, 30, 22, 20, 15, 25, 17,  4,  8,
        31, 27, 13, 23, 21, 19, 16,  7, 26, 12, 18,  6, 11,  5, 10,  9
    };
    return debruijn32[((unsigned int)((val & -(int)val) * 0x077CB531u)) >> 27];
#endif
}

#endif // RTOS_COMPAT_H
