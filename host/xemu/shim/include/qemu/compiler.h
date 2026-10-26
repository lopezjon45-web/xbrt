#ifndef QEMU_COMPILER_H
#define QEMU_COMPILER_H

#define G_GNUC_PRINTF(fmt, args) __attribute__((format(printf, fmt, args)))
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define QEMU_PACKED __attribute__((packed))
#define QEMU_ALWAYS_INLINE __attribute__((always_inline))
#define QEMU_FALLTHROUGH __attribute__((fallthrough))
#define G_NORETURN __declspec(noreturn)
#define glue(x, y) xglue(x, y)
#define xglue(x, y) x##y
#define stringify(s) tostring(s)
#define tostring(s) #s
#define QEMU_BUILD_BUG_MSG(x, msg) _Static_assert(!(x), msg)
#define QEMU_BUILD_BUG_ON(x) QEMU_BUILD_BUG_MSG(x, "not expecting: " #x)
#define HOST_BIG_ENDIAN 0

#endif
