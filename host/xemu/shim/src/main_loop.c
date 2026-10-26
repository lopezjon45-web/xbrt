#include <windows.h>

#include "shim.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "ui/console.h"

/* timeapi.h declares it for the desktop partition only; WindowsApp.lib and onecore.lib export it (api-ms-win-mm-time). */
__declspec(dllimport) UINT WINAPI timeBeginPeriod(UINT period);

static SRWLOCK g_bql = SRWLOCK_INIT;
static __declspec(thread) bool t_bql_held;

void bql_lock_impl(const char *file, int line)
{
    if (t_bql_held) xbrt_refuse(XBRT_REFUSE_FEATURE, "BQL taken twice by one thread at %s:%d", file, line);
    AcquireSRWLockExclusive(&g_bql);
    t_bql_held = true;
}

void bql_unlock(void)
{
    if (!t_bql_held) xbrt_refuse(XBRT_REFUSE_FEATURE, "BQL released by a thread that does not hold it");
    t_bql_held = false;
    ReleaseSRWLockExclusive(&g_bql);
}

bool bql_locked(void) { return t_bql_held; }
int xbrt_xemu_bql_held(void) { return t_bql_held; }

void xbrt_bql_release_if_held(void)
{
    if (t_bql_held) bql_unlock();
}

static int64_t g_qpc_freq, g_virtual_epoch;

static int64_t now_ns(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart / g_qpc_freq * NANOSECONDS_PER_SECOND + c.QuadPart % g_qpc_freq * NANOSECONDS_PER_SECOND / g_qpc_freq;
}

int64_t qemu_clock_get_ns(QEMUClockType type)
{
    int64_t t = now_ns();
    return type == QEMU_CLOCK_VIRTUAL ? t - g_virtual_epoch : t;
}

static SRWLOCK g_timers_lock = SRWLOCK_INIT;
static QEMUTimer *g_pending;
static HANDLE g_clock_wake;

static int64_t realtime_of(const QEMUTimer *ts)
{
    return ts->type == QEMU_CLOCK_VIRTUAL ? ts->expire_time + g_virtual_epoch : ts->expire_time;
}

static void unlink_locked(QEMUTimer *ts)
{
    for (QEMUTimer **p = &g_pending; *p; p = &(*p)->next) {
        if (*p == ts) {
            *p = ts->next;
            break;
        }
    }
    ts->pending = false;
    ts->next = NULL;
}

void timer_init_ns(QEMUTimer *ts, QEMUClockType type, QEMUTimerCB *cb, void *opaque)
{
    *ts = (QEMUTimer){.expire_time = -1, .type = type, .cb = cb, .opaque = opaque};
}

void timer_mod(QEMUTimer *ts, int64_t expire_time)
{
    AcquireSRWLockExclusive(&g_timers_lock);
    if (ts->pending) unlink_locked(ts);
    ts->expire_time = expire_time;
    ts->pending = true;
    QEMUTimer **p = &g_pending;
    while (*p && realtime_of(*p) <= realtime_of(ts)) p = &(*p)->next;
    ts->next = *p;
    *p = ts;
    bool earliest = g_pending == ts;
    ReleaseSRWLockExclusive(&g_timers_lock);
    if (earliest && g_clock_wake) SetEvent(g_clock_wake);
}

void timer_del(QEMUTimer *ts)
{
    AcquireSRWLockExclusive(&g_timers_lock);
    if (ts->pending) unlink_locked(ts);
    ReleaseSRWLockExclusive(&g_timers_lock);
}

bool timer_pending(QEMUTimer *ts) { return ts->pending; }

static QEMUTimer *pop_due_under_bql(DWORD *wait_ms)
{
    AcquireSRWLockExclusive(&g_timers_lock);
    QEMUTimer *ts = g_pending;
    *wait_ms = INFINITE;
    if (ts) {
        int64_t due = realtime_of(ts) - now_ns();
        if (due <= 0) unlink_locked(ts);
        else {
            *wait_ms = (DWORD)((due + SCALE_MS - 1) / SCALE_MS);
            ts = NULL;
        }
    }
    ReleaseSRWLockExclusive(&g_timers_lock);
    return ts;
}

static DWORD WINAPI clock_thread(void *unused)
{
    (void)unused;
    for (;;) {
        DWORD wait_ms;
        bql_lock();
        for (QEMUTimer *ts; (ts = pop_due_under_bql(&wait_ms));) ts->cb(ts->opaque);
        bql_unlock();
        WaitForSingleObject(g_clock_wake, wait_ms);
    }
}

void xbrt_clock_init(void)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpc_freq = f.QuadPart;
    g_virtual_epoch = now_ns();
    /* The APU paces 5.3 ms frame groups with millisecond condition waits; at Windows' default 15.6 ms resolution its
     * sample counter would run at a third of 48 kHz. */
    timeBeginPeriod(1);
}

struct QemuConsole {
    const GraphicHwOps *ops;
    void *opaque;
};

static QemuConsole g_console;
static DisplaySurface g_zero_bpp_surface;
static QEMUTimer g_vblank;
static int64_t g_vblank_period_ns;

/* xemu latches the NV2A's vblank from its UI's display refresh (nv2a_vga_gfx_update); here the clock thread is that
 * refresh. A late tick is dropped rather than made up in a burst. */
static void vblank(void *unused)
{
    (void)unused;
    g_console.ops->gfx_update(g_console.opaque);
    xbrt_count_vblank();
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME), next = g_vblank.expire_time + g_vblank_period_ns;
    timer_mod(&g_vblank, next > now ? next : now + g_vblank_period_ns);
}

void xbrt_clock_start(void)
{
    if (g_console.ops) {
        if (!(g_xbrt_host->vblank_hz > 0))
            xbrt_refuse(XBRT_REFUSE_FEATURE, "a display device without a vblank rate (XbrtHost.vblank_hz)");
        g_vblank_period_ns = (int64_t)(NANOSECONDS_PER_SECOND / g_xbrt_host->vblank_hz);
        timer_init_ns(&g_vblank, QEMU_CLOCK_REALTIME, vblank, NULL);
        timer_mod(&g_vblank, qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + g_vblank_period_ns);
    }
    g_clock_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    HANDLE h = CreateThread(NULL, 0, clock_thread, NULL, CREATE_SUSPENDED, NULL);
    if (!h) xbrt_refuse(XBRT_REFUSE_FEATURE, "CreateThread failed for xbrt.clock (error %lu)", GetLastError());
    SetThreadDescription(h, L"xbrt.clock");
    SetThreadPriority(h, THREAD_PRIORITY_ABOVE_NORMAL);
    xbrt_thread_set_affinity(h);
    ResumeThread(h);
    CloseHandle(h);
}

QemuConsole *graphic_console_init(DeviceState *dev, uint32_t head, const GraphicHwOps *ops, void *opaque)
{
    (void)dev;
    (void)head;
    if (g_console.ops) xbrt_refuse(XBRT_REFUSE_FEATURE, "a second graphic console");
    g_console = (QemuConsole){ops, opaque};
    return &g_console;
}

DisplaySurface *qemu_console_surface(QemuConsole *con)
{
    (void)con;
    return &g_zero_bpp_surface;
}
