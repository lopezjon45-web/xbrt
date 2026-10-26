#include <windows.h>

#include "shim.h"
#include "cpu.h"
#include "gloffscreen.h"
#include "hw/display/vga.h"
#include "hw/xbox/mcpx/apu/dsp/dsp_internal.h"
#include "system/ioport.h"
#include "system/reset.h"
#include "ui/console.h"
#include "ui/xemu-notifications.h"
#include <SDL3/SDL.h>

#define REFUSED(ret, name, params, why)                                                                                \
    ret name params { xbrt_refuse(XBRT_REFUSE_FEATURE, "xemu shim: " #name ": " why); }

REFUSED(void, glGenTextures, (GLsizei n, GLuint *t), "the framebuffer surface is xemu-UI only")
REFUSED(void, glBindTexture, (GLenum a, GLuint t), "the framebuffer surface is xemu-UI only")
REFUSED(void, glTexImage2D, (GLenum a, GLint b, GLint c, GLsizei d, GLsizei e, GLint f, GLenum g, GLenum h, const GLvoid *i),
        "the framebuffer surface is xemu-UI only")
REFUSED(void, glTexParameteri, (GLenum a, GLenum b, GLint c), "the framebuffer surface is xemu-UI only")
REFUSED(void, glDeleteTextures, (GLsizei n, const GLuint *t), "the framebuffer surface is xemu-UI only")
GloContext *glo_context_create(void) { return NULL; }
void glo_set_current(GloContext *c) { (void)c; }

/* dsp.c compares dsp->ops against jit_dsp_ops, so the symbol exists. */
const DSPOps jit_dsp_ops;
REFUSED(void, dsp_jit_init, (DSPState *dsp), "the DSP JIT is not in xbrt")

REFUSED(CPUState *, qemu_get_cpu, (int index), "renderer switch is not supported")
REFUSED(void, async_safe_run_on_cpu, (CPUState *cpu, run_on_cpu_func func, run_on_cpu_data data),
        "renderer switch is not supported")

REFUSED(Object *, qdev_get_machine, (void), "vga_init_io is for ISA VGA")
REFUSED(Object *, object_dynamic_cast, (Object *o, const char *t), "vga_init_io is for ISA VGA")
REFUSED(void, object_unparent, (Object *o), "the VGA chain4 alias needs a legacy address space")
REFUSED(void, qemu_register_reset, (void (*fn)(void *), void *opaque), "vga_init is for ISA VGA")
REFUSED(void, portio_list_init, (PortioList *l, Object *o, const MemoryRegionPortio *c, void *p, const char *n),
        "vga_init is for ISA VGA")
REFUSED(void, portio_list_set_flush_coalesced, (PortioList *l), "vga_init is for ISA VGA")
REFUSED(void, portio_list_add, (PortioList *l, MemoryRegion *as, uint32_t addr), "vga_init is for ISA VGA")

REFUSED(void, qemu_console_resize, (QemuConsole *c, int w, int h), "VGA drawing is not xbrt's presenter")
REFUSED(DisplaySurface *, qemu_create_displaysurface, (int w, int h), "VGA drawing is not xbrt's presenter")
REFUSED(DisplaySurface *, qemu_create_displaysurface_from,
        (int w, int h, pixman_format_code_t f, int linesize, uint8_t *data), "VGA drawing is not xbrt's presenter")
REFUSED(pixman_format_code_t, qemu_default_pixman_format, (int bpp, bool native_endian),
        "VGA drawing is not xbrt's presenter")
REFUSED(void, dpy_gfx_update, (QemuConsole *c, int x, int y, int w, int h), "VGA drawing is not xbrt's presenter")
REFUSED(void, dpy_gfx_update_full, (QemuConsole *c), "VGA drawing is not xbrt's presenter")
REFUSED(void, dpy_gfx_replace_surface, (QemuConsole *c, DisplaySurface *s), "VGA drawing is not xbrt's presenter")
REFUSED(bool, dpy_gfx_check_format, (QemuConsole *c, pixman_format_code_t f), "VGA drawing is not xbrt's presenter")
REFUSED(void, dpy_text_cursor, (QemuConsole *c, int x, int y), "VGA text mode is not xbrt's presenter")
REFUSED(void, dpy_text_update, (QemuConsole *c, int x, int y, int w, int h), "VGA text mode is not xbrt's presenter")
REFUSED(void, dpy_text_resize, (QemuConsole *c, int w, int h), "VGA text mode is not xbrt's presenter")
REFUSED(DirtyBitmapSnapshot *, memory_region_snapshot_and_clear_dirty,
        (MemoryRegion *mr, hwaddr addr, hwaddr size, unsigned client), "VGA drawing is not xbrt's presenter")
REFUSED(bool, memory_region_snapshot_get_dirty, (MemoryRegion *mr, DirtyBitmapSnapshot *s, hwaddr addr, hwaddr size),
        "VGA drawing is not xbrt's presenter")

bool SDL_Init(uint32_t flags)
{
    (void)flags;
    return false;
}
const char *SDL_GetError(void) { return "xbrt has no audio output yet"; }
REFUSED(SDL_AudioStream *, SDL_OpenAudioDeviceStream, (SDL_AudioDeviceID d, const SDL_AudioSpec *s, void *c, void *u),
        "no audio output yet")
REFUSED(SDL_AudioDeviceID, SDL_GetAudioStreamDevice, (SDL_AudioStream *s), "no audio output yet")
REFUSED(bool, SDL_GetAudioDeviceFormat, (SDL_AudioDeviceID d, SDL_AudioSpec *s, int *f), "no audio output yet")
REFUSED(bool, SDL_ResumeAudioDevice, (SDL_AudioDeviceID d), "no audio output yet")
REFUSED(void, SDL_DestroyAudioStream, (SDL_AudioStream *s), "no audio output yet")
REFUSED(bool, SDL_SetAudioStreamGain, (SDL_AudioStream *s, float g), "no audio output yet")
REFUSED(bool, SDL_PutAudioStreamData, (SDL_AudioStream *s, const void *b, int n), "no audio output yet")
REFUSED(int, SDL_GetAudioStreamQueued, (SDL_AudioStream *s), "no audio output yet")
int SDL_GetNumLogicalCPUCores(void)
{
    SYSTEM_INFO info;
    GetNativeSystemInfo(&info);
    return (int)info.dwNumberOfProcessors;
}
uint64_t SDL_GetTicks(void) { return GetTickCount64(); }

enum vga_retrace_method vga_retrace_method = VGA_RETRACE_DUMB;
void *qemu_ram_block_by_name(const char *name)
{
    (void)name;
    return NULL;
}
void vmstate_register_ram(MemoryRegion *mr, DeviceState *dev)
{
    (void)mr;
    (void)dev;
}

void xemu_queue_notification(const char *msg)
{
    char *line = g_strdup_printf("xemu: %s", msg);
    g_xbrt_host->log(line);
    g_free(line);
}

void xemu_queue_error_message(const char *msg)
{
    char *line = g_strdup_printf("xemu error: %s", msg);
    g_xbrt_host->log(line);
    g_free(line);
}
