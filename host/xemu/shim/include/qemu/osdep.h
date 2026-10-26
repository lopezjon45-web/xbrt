#ifndef QEMU_OSDEP_H
#define QEMU_OSDEP_H

#ifndef __clang__
#error "the xemu side builds with clang-cl only: upstream uses case ranges, statement expressions and constructors"
#endif

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "glib.h"
#include "qemu/compiler.h"
#include "qemu/typedefs.h"
#include "xbrt_xemu.h"

typedef uint64_t hwaddr;
#define HWADDR_PRIx PRIx64
#define HWADDR_MAX UINT64_MAX
typedef uint64_t ram_addr_t;
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef intptr_t ssize_t;
#endif

__declspec(noreturn) void xbrt_refuse(XbrtRefusal kind, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
__declspec(noreturn) void xbrt_shim_exit(int code, const char *file, int line);
__declspec(noreturn) void xbrt_shim_abort(const char *file, int line);
#define exit(code) xbrt_shim_exit((code), __FILE__, __LINE__)
#define abort() xbrt_shim_abort(__FILE__, __LINE__)

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define ROUND_UP(n, d) (((n) + (d) - 1) & -(0 ? (n) : (d)))
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define QEMU_ALIGN_DOWN(n, m) ((n) / (m) * (m))
#define QEMU_ALIGN_UP(n, m) QEMU_ALIGN_DOWN((n) + (m) - 1, (m))
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

#include "qemu/host-utils.h"
#include "qemu/bswap.h"
#include "qemu/atomic.h"

#endif
