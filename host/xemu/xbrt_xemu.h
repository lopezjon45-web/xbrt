/* The C ABI between xbrt (host/xemu/machine.cpp, cl.exe, C++) and the xemu side (host/xemu/shim and the vendored
 * external/xemu, clang-cl, C). No QEMU type crosses it, and cl.exe never has the shim's include directory, so host
 * code cannot include a QEMU header. */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum XbrtRefusal {
    XBRT_REFUSE_DEVICE_REGISTER,   /* a hole in a BAR, an unaligned access, a physical access outside RAM */
    XBRT_REFUSE_FEATURE,           /* a refused shim call reached, an assert, abort or exit in device code, a map change */
} XbrtRefusal;

typedef enum XbrtRenderer {
    XBRT_RENDERER_NULL,
    XBRT_RENDERER_PROBE,
} XbrtRenderer;

/* What the xemu side needs from xbrt, given once to xbrt_xemu_create and kept. */
typedef struct XbrtHost {
    uint8_t *ram;              /* host pointer of guest physical 0 */
    uint32_t ram_bytes;
    uint64_t device_cores;     /* affinity of every thread the xemu side creates */
    unsigned voice_workers;    /* the APU's VP mixing threads; 0 lets upstream pick one per logical core */
    double vblank_hz;
    XbrtRenderer renderer;
    /* A device's interrupt line changed level. Called with the BQL held, from any thread: it stores the level and
     * wakes the interrupt thread, and never calls guest code or blocks. */
    void (*irq_level)(unsigned line, int level);
    /* Ends the session with a named refusal and does not return. Called without the BQL, so no device thread or ISR
     * waits on the thread that is ending the session. */
    void (*refuse)(XbrtRefusal kind, const char *what);
    void (*log)(const char *line);
} XbrtHost;

/* One PCI BAR a device registered. `device` is its aperture name in core/devices.h. Lines are numbered in device
 * creation order, and every BAR of a device carries that device's line. */
typedef struct XbrtBar {
    const char *device;
    uint8_t index;
    uint8_t irq_line;
    uint8_t ram_alias;   /* the BAR is all of board RAM from physical 0 (the NV2A's BAR1), so nothing to dispatch */
    uint32_t size;
} XbrtBar;

/* Creates every device (QOM realize with the BQL held), then freezes and flattens their memory maps. Writes the BARs
 * in creation order and returns how many. No line has risen yet. Once per process; refuses on any failure. */
unsigned xbrt_xemu_create(const XbrtHost *host, XbrtBar *bars, unsigned max_bars);

/* Resets every device, starts the clock thread, and runs the machine: each vm change state handler with running and
 * RUN_STATE_RUNNING, which un-pauses the APU frame thread. A line may rise from here. */
void xbrt_xemu_run(void);

/* A guest access the trap router decoded inside bars[bar]: `offset` from the BAR base, size 1, 2 or 4. Takes the BQL
 * around the device callback as QEMU's memory core does for a vCPU. A hole or an unaligned access is a refusal. */
uint32_t xbrt_xemu_read(unsigned bar, uint32_t offset, unsigned size);
void xbrt_xemu_write(unsigned bar, uint32_t offset, unsigned size, uint32_t value);

int xbrt_xemu_bql_held(void);

/* Totals since xbrt_xemu_run, read without a lock (each is written by one thread). APU frames per second and
 * utilization are upstream's own once-a-second figures (mcpx_apu_get_debug_info). */
typedef struct XbrtCounters {
    uint64_t nv2a_methods;   /* entries in PGRAPH's method log (a run of ARRAY_ELEMENT16 counts its calls) */
    uint64_t nv2a_draws;     /* SET_BEGIN_END END: begin/end pairs */
    uint64_t nv2a_flips;     /* FLIP_STALL */
    uint64_t vblanks;
    int apu_frames_per_second;
    float apu_utilization;
} XbrtCounters;

void xbrt_xemu_counters(XbrtCounters *out);

#ifdef __cplusplus
}
#endif
