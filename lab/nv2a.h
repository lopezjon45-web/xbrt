#pragma once

// One NV2A DMA channel built by hand, as a driver does before its first push buffer: no pbkit, no D3D. Register
// offsets and fields are upstream xemu's nv2a_regs.h; the NV097 method numbers are nxdk's pbkit nv_regs.h.

#include "lab.h"

#define NV2A 0xFD000000u
#define NV_PMC 0x000000u
#define NV_PFIFO 0x002000u
#define NV_PTIMER 0x009000u
#define NV_PGRAPH 0x400000u
#define NV_PCRTC 0x600000u
#define NV_PRAMIN 0x700000u
#define NV_USER 0x800000u

#define PMC_BOOT_0 0x000u
#define PMC_INTR_0 0x100u
#define PMC_INTR_0_PGRAPH (1u << 12)
#define PMC_INTR_0_PTIMER (1u << 20)
#define PMC_INTR_0_PCRTC (1u << 24)
#define PMC_INTR_EN_0 0x140u
#define PMC_INTR_EN_0_HARDWARE 1u

#define PFIFO_RAMHT 0x210u
#define PFIFO_MODE 0x504u
#define PFIFO_CACHE1_PUSH0 0x1200u
#define PFIFO_CACHE1_PUSH1 0x1204u
#define PFIFO_CACHE1_PUSH1_MODE_DMA (1u << 8)
#define PFIFO_CACHE1_DMA_PUSH 0x1220u
#define PFIFO_CACHE1_DMA_INSTANCE 0x122Cu
#define PFIFO_CACHE1_DMA_GET 0x1244u
#define PFIFO_CACHE1_PULL0 0x1250u

#define PGRAPH_INTR 0x100u
#define PGRAPH_INTR_CONTEXT_SWITCH (1u << 12)
#define PGRAPH_INTR_EN 0x140u
#define PGRAPH_CTX_CONTROL 0x144u
#define PGRAPH_CTX_CONTROL_CHID_VALID (1u << 16)
#define PGRAPH_CTX_USER 0x148u
#define PGRAPH_FIFO 0x720u
#define PGRAPH_COLORCLEARVALUE 0x186Cu

#define PCRTC_INTR_0 0x100u
#define PCRTC_INTR_EN_0 0x140u
#define PCRTC_VBLANK 1u

#define PTIMER_INTR_0 0x100u
#define PTIMER_INTR_EN_0 0x140u
#define PTIMER_ALARM 1u
#define PTIMER_TIME_0 0x400u
#define PTIMER_ALARM_0 0x420u

#define USER_DMA_PUT 0x40u
#define USER_DMA_GET 0x44u

#define DMA_IN_MEMORY_CLASS 0x3Du
#define DMA_TARGET_PCI 0x00020000u
#define RAMHT_ENGINE_GRAPHICS 0x00010000u
#define RAMHT_STATUS_VALID 0x80000000u

#define NV097_SET_OBJECT 0x0000u
#define NV097_NO_OPERATION 0x0100u
#define NV097_SET_CONTEXT_DMA_SEMAPHORE 0x01A4u
#define NV097_SET_SEMAPHORE_OFFSET 0x1D6Cu
#define NV097_BACK_END_WRITE_SEMAPHORE_RELEASE 0x1D70u
#define NV097_SET_ZSTENCIL_CLEAR_VALUE 0x1D8Cu
#define NV097_SET_COLOR_CLEAR_VALUE 0x1D90u
#define KELVIN_CLASS 0x97u

// RAMIN: a 4 KB hash table, then the objects. Handles below 512 hash to themselves on channel 0.
#define RAMHT_OFFSET 0x10000u
#define DMA_PUSH_BUFFER 0x20000u
#define DMA_SEMAPHORE 0x20010u
#define KELVIN_OBJECT 0x20020u
#define H_KELVIN 0x11u
#define H_SEMAPHORE 0x12u

static inline ULONG nv_rd(ULONG offset) { return *(volatile ULONG *)(NV2A + offset); }
static inline void nv_wr(ULONG offset, ULONG value) { *(volatile ULONG *)(NV2A + offset) = value; }

static inline ULONG nv_method(ULONG subchannel, ULONG method, ULONG count)
{
    return count << 18 | subchannel << 13 | method;
}

// pfifo.c's hash: the handle folded in 11-bit pieces (a 4 KB table), channel 0.
static inline ULONG nv_ramht_hash(ULONG handle)
{
    ULONG hash = 0;
    for (; handle; handle >>= 11) hash ^= handle & 0x7FFu;
    return hash;
}

static inline void nv_dma_object(ULONG instance, ULONG physical, ULONG limit)
{
    nv_wr(NV_PRAMIN + instance, DMA_IN_MEMORY_CLASS | DMA_TARGET_PCI | (physical & 0xFFFu) << 20);
    nv_wr(NV_PRAMIN + instance + 4, limit);
    nv_wr(NV_PRAMIN + instance + 8, physical & ~0xFFFu);
}

static inline void nv_ramht_entry(ULONG handle, ULONG instance)
{
    ULONG at = NV_PRAMIN + RAMHT_OFFSET + nv_ramht_hash(handle) * 8;
    nv_wr(at, handle);
    nv_wr(at + 4, instance >> 4 | RAMHT_ENGINE_GRAPHICS | RAMHT_STATUS_VALID);
}

// Channel 0 in DMA mode over a push buffer at `push_buffer` (physical, page aligned) of `limit` + 1 bytes, with a
// Kelvin object and a semaphore DMA object at `semaphore` (physical). PGRAPH's channel is left invalid when
// `context_valid` is 0, so the first SET_OBJECT raises the context-switch interrupt, as a cold D3D start does.
static inline void nv_channel_setup(ULONG push_buffer, ULONG limit, ULONG semaphore, int context_valid)
{
    nv_dma_object(DMA_PUSH_BUFFER, push_buffer, limit);
    nv_dma_object(DMA_SEMAPHORE, semaphore, 0xFFF);
    nv_wr(NV_PRAMIN + KELVIN_OBJECT, KELVIN_CLASS);
    nv_ramht_entry(H_KELVIN, KELVIN_OBJECT);
    nv_ramht_entry(H_SEMAPHORE, DMA_SEMAPHORE);
    nv_wr(NV_PFIFO + PFIFO_RAMHT, (RAMHT_OFFSET >> 12) << 4);
    nv_wr(NV_PFIFO + PFIFO_MODE, 1);
    nv_wr(NV_PFIFO + PFIFO_CACHE1_PUSH1, PFIFO_CACHE1_PUSH1_MODE_DMA);
    nv_wr(NV_PFIFO + PFIFO_CACHE1_DMA_INSTANCE, DMA_PUSH_BUFFER >> 4);
    nv_wr(NV_PFIFO + PFIFO_CACHE1_DMA_PUSH, 1);
    nv_wr(NV_PFIFO + PFIFO_CACHE1_PUSH0, 1);
    nv_wr(NV_PFIFO + PFIFO_CACHE1_PULL0, 1);
    if (context_valid) {
        nv_wr(NV_PGRAPH + PGRAPH_CTX_USER, 0);
        nv_wr(NV_PGRAPH + PGRAPH_CTX_CONTROL, PGRAPH_CTX_CONTROL_CHID_VALID);
    }
    nv_wr(NV_PGRAPH + PGRAPH_FIFO, 1);
}

// Milliseconds in 32-bit arithmetic: the labs link no CRT, so a 64-bit division (__aulldiv) has no body. Spans stay
// under a few seconds, which fits 32 bits of the performance counter.
static ULONG lab_freq_khz;

static inline void lab_clock_init(void) { lab_freq_khz = (ULONG)KeQueryPerformanceFrequency() / 1000u; }
static inline ULONG lab_elapsed_ms(ULONGLONG since) { return (ULONG)(KeQueryPerformanceCounter() - since) / lab_freq_khz; }
