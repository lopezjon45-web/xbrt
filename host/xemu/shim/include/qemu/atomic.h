#ifndef QEMU_ATOMIC_H
#define QEMU_ATOMIC_H

#define qatomic_read(ptr) __atomic_load_n(ptr, __ATOMIC_RELAXED)
#define qatomic_set(ptr, i) __atomic_store_n(ptr, i, __ATOMIC_RELAXED)
#define qatomic_load_acquire(ptr) __atomic_load_n(ptr, __ATOMIC_ACQUIRE)
#define qatomic_store_release(ptr, i) __atomic_store_n(ptr, i, __ATOMIC_RELEASE)
#define qatomic_xchg(ptr, i) __atomic_exchange_n(ptr, i, __ATOMIC_SEQ_CST)
#define qatomic_or(ptr, n) ((void)__atomic_fetch_or(ptr, n, __ATOMIC_SEQ_CST))
#define qatomic_and(ptr, n) ((void)__atomic_fetch_and(ptr, n, __ATOMIC_SEQ_CST))
#define qatomic_inc(ptr) ((void)__atomic_fetch_add(ptr, 1, __ATOMIC_SEQ_CST))
#define qatomic_dec(ptr) ((void)__atomic_fetch_sub(ptr, 1, __ATOMIC_SEQ_CST))
#define smp_mb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define smp_wmb() __atomic_thread_fence(__ATOMIC_RELEASE)
#define smp_rmb() __atomic_thread_fence(__ATOMIC_ACQUIRE)

#endif
