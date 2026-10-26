#ifndef QEMU_BITMAP_H
#define QEMU_BITMAP_H

#include "qemu/osdep.h"

#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define BITS_TO_LONGS(n) DIV_ROUND_UP((n), BITS_PER_LONG)
#define DECLARE_BITMAP(name, bits) unsigned long name[BITS_TO_LONGS(bits)]

static inline int test_bit(long nr, const unsigned long *addr)
{
    return 1 & (int)(addr[nr / BITS_PER_LONG] >> (nr % BITS_PER_LONG));
}

static inline void bitmap_set(unsigned long *map, long start, long nr)
{
    for (long i = start; i < start + nr; i++) map[i / BITS_PER_LONG] |= 1ul << (i % BITS_PER_LONG);
}

static inline void bitmap_zero(unsigned long *map, long nbits)
{
    memset(map, 0, BITS_TO_LONGS(nbits) * sizeof(unsigned long));
}

#endif
