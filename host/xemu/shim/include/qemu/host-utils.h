#ifndef QEMU_HOST_UTILS_H
#define QEMU_HOST_UTILS_H

/* a * b / c in two 64-bit steps, QEMU's form for hosts without CONFIG_INT128: an unsigned __int128 division would
 * call __udivti3, which link.exe has no library for. */
static inline uint64_t muldiv64_rounding(uint64_t a, uint32_t b, uint32_t c, bool round_up)
{
    uint64_t rl = (a & 0xFFFFFFFFu) * b;
    if (round_up) rl += c - 1;
    uint64_t rh = (a >> 32) * b + (rl >> 32);
    uint64_t high = rh / c;
    uint64_t low = (((rh % c) << 32) + (rl & 0xFFFFFFFFu)) / c;
    return (high << 32) + low;
}

static inline uint64_t muldiv64(uint64_t a, uint32_t b, uint32_t c) { return muldiv64_rounding(a, b, c, false); }
static inline uint64_t muldiv64_round_up(uint64_t a, uint32_t b, uint32_t c) { return muldiv64_rounding(a, b, c, true); }

static inline int clz32(uint32_t v) { return v ? __builtin_clz(v) : 32; }
static inline int clz64(uint64_t v) { return v ? __builtin_clzll(v) : 64; }
static inline int ctz32(uint32_t v) { return v ? __builtin_ctz(v) : 32; }
static inline int ctz64(uint64_t v) { return v ? __builtin_ctzll(v) : 64; }
static inline int ctpop32(uint32_t v) { return __builtin_popcount(v); }

static inline uint64_t pow2ceil(uint64_t value)
{
    int n = clz64(value - 1);
    if (!n) return !value;
    return 0x8000000000000000ull >> (n - 1);
}

#endif
