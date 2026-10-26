#ifndef XBRT_SHIM_CPU_H
#define XBRT_SHIM_CPU_H

#include "qemu/osdep.h"
#include "exec/target_page.h"

typedef struct {
    void *host_ptr;
} run_on_cpu_data;
#define RUN_ON_CPU_HOST_PTR(p) ((run_on_cpu_data){.host_ptr = (p)})
typedef void (*run_on_cpu_func)(CPUState *cpu, run_on_cpu_data data);

CPUState *qemu_get_cpu(int index);
void async_safe_run_on_cpu(CPUState *cpu, run_on_cpu_func func, run_on_cpu_data data);

#endif
