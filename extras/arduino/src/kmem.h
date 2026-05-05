//---------------------------------------------------------------------------
// Copyright 2025. All rights reserved.
//
// Kernel-internal mem helpers. Avoids a dependency on <string.h> so that
// bare-metal ports can build against toolchains that do not ship a libc
// (e.g. Ubuntu's gcc-riscv64-unknown-elf).
//---------------------------------------------------------------------------
#ifndef RTOS_KMEM_H
#define RTOS_KMEM_H

#include <stddef.h>
#include <stdint.h>

// Simple byte-wise implementations of memcpy and memset. These are not 
// optimized for performance, but they are small and portable. They are 
// intended for use in the kernel, where we may not have access to a 
// full C library, and where performance is not critical.

//---------------------------------------------------------------------------
// kernel memcpy and memset
//---------------------------------------------------------------------------
static inline void rtos_kmemcpy(void *dst, const void *src, size_t n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    while (n--) 
    {
        *d++ = *s++;
    }
}

static inline void rtos_kmemset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;

    while (n--) 
    {
        *d++ = (unsigned char)c;
    }
}

#endif // RTOS_KMEM_H
