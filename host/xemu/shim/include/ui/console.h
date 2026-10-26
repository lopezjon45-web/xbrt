#ifndef UI_CONSOLE_H
#define UI_CONSOLE_H

#include "qemu/osdep.h"

typedef uint32_t pixman_format_code_t;
typedef uint32_t console_ch_t;

enum qemu_color_names {
    QEMU_COLOR_BLACK = 0,
    QEMU_COLOR_BLUE = 1,
    QEMU_COLOR_GREEN = 2,
    QEMU_COLOR_CYAN = 3,
    QEMU_COLOR_RED = 4,
    QEMU_COLOR_MAGENTA = 5,
    QEMU_COLOR_YELLOW = 6,
    QEMU_COLOR_WHITE = 7
};
#define ATTR2CHTYPE(c, fg, bg, bold) ((bold) << 21 | (bg) << 11 | (fg) << 8 | (c))

static inline void console_write_ch(console_ch_t *dest, uint32_t ch) { *dest = ch; }

struct GraphicHwOps {
    int (*get_flags)(void *opaque);
    void (*invalidate)(void *opaque);
    void (*gfx_update)(void *opaque);
    bool gfx_update_async;
    void (*text_update)(void *opaque, console_ch_t *text);
    void (*ui_info)(void *opaque, uint32_t head, void *info);
    void (*gl_block)(void *opaque, bool block);
};

struct DisplaySurface {
    pixman_format_code_t format;
    int width, height, stride;
    void *data;
};

QemuConsole *graphic_console_init(DeviceState *dev, uint32_t head, const GraphicHwOps *ops, void *opaque);
DisplaySurface *qemu_console_surface(QemuConsole *con);
void qemu_console_resize(QemuConsole *con, int width, int height);
DisplaySurface *qemu_create_displaysurface(int width, int height);
DisplaySurface *qemu_create_displaysurface_from(int width, int height, pixman_format_code_t format, int linesize,
                                                uint8_t *data);
pixman_format_code_t qemu_default_pixman_format(int bpp, bool native_endian);
void dpy_gfx_update(QemuConsole *con, int x, int y, int w, int h);
void dpy_gfx_update_full(QemuConsole *con);
void dpy_gfx_replace_surface(QemuConsole *con, DisplaySurface *surface);
bool dpy_gfx_check_format(QemuConsole *con, pixman_format_code_t format);
void dpy_text_cursor(QemuConsole *con, int x, int y);
void dpy_text_update(QemuConsole *con, int x, int y, int w, int h);
void dpy_text_resize(QemuConsole *con, int w, int h);

static inline int surface_is_allocated(DisplaySurface *s) { return s && s->data != NULL; }
static inline int surface_stride(DisplaySurface *s) { return s->stride; }
static inline void *surface_data(DisplaySurface *s) { return s->data; }
static inline int surface_width(DisplaySurface *s) { return s->width; }
static inline int surface_height(DisplaySurface *s) { return s->height; }
static inline int surface_bits_per_pixel(DisplaySurface *s) { return (int)((s->format >> 24) & 0xFF); }
static inline int surface_bytes_per_pixel(DisplaySurface *s) { return (surface_bits_per_pixel(s) + 7) / 8; }

#endif
