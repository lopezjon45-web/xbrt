#include <windows.h>

#include "shim.h"
#include "qemu/thread.h"

_Static_assert(sizeof(QemuMutex) == sizeof(SRWLOCK), "QemuMutex holds an SRWLOCK");
_Static_assert(sizeof(QemuCond) == sizeof(CONDITION_VARIABLE), "QemuCond holds a CONDITION_VARIABLE");

void qemu_mutex_init(QemuMutex *m) { InitializeSRWLock((SRWLOCK *)m); }
void qemu_mutex_destroy(QemuMutex *m) { (void)m; }
void qemu_mutex_lock(QemuMutex *m) { AcquireSRWLockExclusive((SRWLOCK *)m); }
void qemu_mutex_unlock(QemuMutex *m) { ReleaseSRWLockExclusive((SRWLOCK *)m); }

void qemu_cond_init(QemuCond *c) { InitializeConditionVariable((CONDITION_VARIABLE *)c); }
void qemu_cond_destroy(QemuCond *c) { (void)c; }
void qemu_cond_signal(QemuCond *c) { WakeConditionVariable((CONDITION_VARIABLE *)c); }
void qemu_cond_broadcast(QemuCond *c) { WakeAllConditionVariable((CONDITION_VARIABLE *)c); }

void qemu_cond_wait(QemuCond *c, QemuMutex *m)
{
    SleepConditionVariableSRW((CONDITION_VARIABLE *)c, (SRWLOCK *)m, INFINITE, 0);
}

bool qemu_cond_timedwait(QemuCond *c, QemuMutex *m, int ms)
{
    return SleepConditionVariableSRW((CONDITION_VARIABLE *)c, (SRWLOCK *)m, ms < 0 ? 0 : (DWORD)ms, 0) != 0;
}

void qemu_event_init(QemuEvent *e, bool init) { e->handle = CreateEventW(NULL, TRUE, init, NULL); }
void qemu_event_destroy(QemuEvent *e) { CloseHandle(e->handle); }
void qemu_event_set(QemuEvent *e) { SetEvent(e->handle); }
void qemu_event_reset(QemuEvent *e) { ResetEvent(e->handle); }
void qemu_event_wait(QemuEvent *e) { WaitForSingleObject(e->handle, INFINITE); }

typedef struct Start {
    void *(*fn)(void *);
    void *arg;
} Start;

static DWORD WINAPI thread_main(void *p)
{
    Start start = *(Start *)p;
    g_free(p);
    start.fn(start.arg);
    return 0;
}

void qemu_thread_create(QemuThread *t, const char *name, void *(*fn)(void *), void *arg, int mode)
{
    Start *start = g_malloc(sizeof *start);
    *start = (Start){fn, arg};
    HANDLE h = CreateThread(NULL, 0, thread_main, start, CREATE_SUSPENDED, NULL);
    if (!h) xbrt_refuse(XBRT_REFUSE_FEATURE, "CreateThread failed for %s (error %lu)", name, GetLastError());
    wchar_t wide[64];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, ARRAYSIZE(wide));
    SetThreadDescription(h, wide);
    xbrt_thread_set_affinity(h);
    ResumeThread(h);
    if (mode == QEMU_THREAD_DETACHED) {
        CloseHandle(h);
        h = NULL;
    }
    t->handle = h;
}

void *qemu_thread_join(QemuThread *t)
{
    WaitForSingleObject(t->handle, INFINITE);
    CloseHandle(t->handle);
    t->handle = NULL;
    return NULL;
}

void xbrt_thread_set_affinity(void *thread)
{
    if (g_xbrt_host->device_cores) SetThreadAffinityMask(thread, (DWORD_PTR)g_xbrt_host->device_cores);
}
