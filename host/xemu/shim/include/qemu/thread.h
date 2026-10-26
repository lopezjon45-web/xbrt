#ifndef QEMU_THREAD_H
#define QEMU_THREAD_H

#include "qemu/osdep.h"

typedef struct QemuMutex {
    void *srw;
} QemuMutex;
typedef struct QemuCond {
    void *cv;
} QemuCond;
typedef struct QemuEvent {
    void *handle;
} QemuEvent;
typedef struct QemuThread {
    void *handle;
} QemuThread;

#define QEMU_THREAD_JOINABLE 0
#define QEMU_THREAD_DETACHED 1

void qemu_mutex_init(QemuMutex *m);
void qemu_mutex_destroy(QemuMutex *m);
void qemu_mutex_lock(QemuMutex *m);
void qemu_mutex_unlock(QemuMutex *m);
void qemu_cond_init(QemuCond *c);
void qemu_cond_destroy(QemuCond *c);
void qemu_cond_signal(QemuCond *c);
void qemu_cond_broadcast(QemuCond *c);
void qemu_cond_wait(QemuCond *c, QemuMutex *m);
bool qemu_cond_timedwait(QemuCond *c, QemuMutex *m, int ms);
void qemu_event_init(QemuEvent *e, bool init);
void qemu_event_destroy(QemuEvent *e);
void qemu_event_set(QemuEvent *e);
void qemu_event_reset(QemuEvent *e);
void qemu_event_wait(QemuEvent *e);
void qemu_thread_create(QemuThread *t, const char *name, void *(*fn)(void *), void *arg, int mode);
void *qemu_thread_join(QemuThread *t);
static inline void rcu_register_thread(void) {}
static inline void rcu_unregister_thread(void) {}

#endif
