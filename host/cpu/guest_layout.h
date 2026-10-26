// The per-thread block the guest reaches through fs:, at the Xbox's own offsets, and the host-only page behind it.
// Private to host/cpu. Offsets are the Xbox kernel's as Cxbx-Reloaded records them (src/core/kernel/common/types.h,
// KPCR, KPRCB, KTHREAD, ETHREAD; EmuFS.cpp:52-80 lists the ones titles read). No Windows header: emit.cpp is built
// from this file alone, so the gate bytes can be decoded on the Mac.
#pragma once

#include <cstddef>
#include <cstdint>

namespace xbrt {
class GuestThread;
}

namespace xbrt::detail {

struct SpawnArgs;

#pragma pack(push, 1)
struct XboxNtTib {   // KPCR + 0x00
    uint32_t ExceptionList;   // fs:[0x00]
    uint32_t StackBase;       // fs:[0x04]
    uint32_t StackLimit;      // fs:[0x08]
    uint32_t SubSystemTib;
    uint32_t FiberData;
    uint32_t ArbitraryUserPointer;
    uint32_t Self;   // fs:[0x18]
};

struct XboxKprcb {   // KPCR + 0x28
    uint32_t CurrentThread;   // fs:[0x28]
    uint32_t NextThread, IdleThread;
    uint32_t Unknown1[7];
    uint32_t DpcListHead[2];    // KPRCB + 0x28
    uint32_t DpcRoutineActive;  // KPRCB + 0x30, fs:[0x58]; DSOUND reads it
    uint8_t pad0[0x250 - 0x34];
    // KPRCB + 0x250. Zero is a retail console with no debug monitor, on which XAPI's start-up rewrites the GDT from
    // the kernel image at 0x80010000; xbrt has neither, so every thread points it at one empty debug monitor block.
    uint32_t DebugMonitorData;
    uint8_t pad1[0x258 - 0x254];   // Cxbx sizes the KPRCB at 0x258
};

struct XboxKpcr {
    XboxNtTib NtTib;   // 0x00
    uint32_t SelfPcr;  // 0x1C
    uint32_t Prcb;     // 0x20: fs:[0x20]
    uint8_t Irql;      // 0x24: fs:[0x24]
    uint8_t pad[3];
    XboxKprcb PrcbData;   // 0x28
};

struct XboxKthread {
    uint8_t Header[0x10];          // DISPATCHER_HEADER
    uint32_t MutantListHead[2];
    uint32_t KernelTime;           // 0x18
    uint32_t StackBase;            // 0x1C
    uint32_t StackLimit;           // 0x20
    uint32_t KernelStack;          // 0x24
    uint32_t TlsData;              // 0x28: fs:[0x28] -> +0x28 is how a title reaches its TLS
    uint8_t State;                 // 0x2C
    uint8_t Alerted[2];
    uint8_t Alertable;
    uint8_t NpxState;              // 0x30
    uint8_t Saturation;
    uint8_t Priority;              // 0x32
    uint8_t Padding;
    uint8_t pad0[0x70 - 0x34];     // ApcState, the wait fields, the queue
    uint8_t BasePriority;          // 0x70
    uint8_t DecrementCount, PriorityDecrement, DisableBoost;
    uint8_t NpxIrql;               // 0x74
    uint8_t SuspendCount;          // 0x75
    uint8_t Preempted;
    uint8_t HasTerminated;         // 0x77
    uint8_t pad1[0x110 - 0x78];    // Queue, the timer, the suspend APC and semaphore, ThreadListEntry
};

struct XboxEthread {
    XboxKthread Tcb;           // 0x000
    uint64_t CreateTime;       // 0x110
    uint64_t ExitTime;         // 0x118
    uint32_t ExitStatus;       // 0x120
    uint32_t ReaperLink[2];    // 0x124
    uint32_t UniqueThread;     // 0x12C: KeGetCurrentThread()->UniqueThread is the thread id
    uint32_t StartAddress;     // 0x130
    uint32_t IrpList[2];       // 0x134
    uint32_t DebugData;        // 0x13C
};
#pragma pack(pop)

static_assert(offsetof(XboxKpcr, NtTib.ExceptionList) == 0x00);
static_assert(offsetof(XboxKpcr, NtTib.StackBase) == 0x04);
static_assert(offsetof(XboxKpcr, NtTib.StackLimit) == 0x08);
static_assert(offsetof(XboxKpcr, NtTib.ArbitraryUserPointer) == 0x14);
static_assert(offsetof(XboxKpcr, NtTib.Self) == 0x18);
static_assert(offsetof(XboxKpcr, SelfPcr) == 0x1C);
static_assert(offsetof(XboxKpcr, Prcb) == 0x20);
static_assert(offsetof(XboxKpcr, Irql) == 0x24);
static_assert(offsetof(XboxKpcr, PrcbData.CurrentThread) == 0x28);
static_assert(offsetof(XboxKpcr, PrcbData.NextThread) == 0x2C);
static_assert(offsetof(XboxKpcr, PrcbData.IdleThread) == 0x30);
static_assert(offsetof(XboxKpcr, PrcbData.DpcListHead) == 0x50);
static_assert(offsetof(XboxKpcr, PrcbData.DpcRoutineActive) == 0x58);
static_assert(offsetof(XboxKprcb, DebugMonitorData) == 0x250);
static_assert(sizeof(XboxKprcb) == 0x258);
static_assert(sizeof(XboxKpcr) == 0x280);
static_assert(offsetof(XboxKthread, KernelTime) == 0x18);
static_assert(offsetof(XboxKthread, StackBase) == 0x1C);
static_assert(offsetof(XboxKthread, StackLimit) == 0x20);
static_assert(offsetof(XboxKthread, KernelStack) == 0x24);
static_assert(offsetof(XboxKthread, TlsData) == 0x28);
static_assert(offsetof(XboxKthread, State) == 0x2C);
static_assert(offsetof(XboxKthread, NpxState) == 0x30);
static_assert(offsetof(XboxKthread, Priority) == 0x32);
static_assert(offsetof(XboxKthread, BasePriority) == 0x70);
static_assert(offsetof(XboxKthread, SuspendCount) == 0x75);
static_assert(offsetof(XboxKthread, HasTerminated) == 0x77);
static_assert(sizeof(XboxKthread) == 0x110);
static_assert(offsetof(XboxEthread, CreateTime) == 0x110);
static_assert(offsetof(XboxEthread, ExitStatus) == 0x120);
static_assert(offsetof(XboxEthread, UniqueThread) == 0x12C);
static_assert(offsetof(XboxEthread, StartAddress) == 0x130);
static_assert(sizeof(XboxEthread) == 0x140);

inline constexpr uint8_t kKthreadRunning = 2;   // KTHREAD_STATE

// The ThreadBlock: two pages in the runtime region; r15 holds its base while the thread runs guest code.
inline constexpr uint32_t kThreadBlockSize = 0x2000;
inline constexpr uint32_t kEthreadOffset = 0x400;
inline constexpr uint32_t kHostPartOffset = 0x1000;
inline constexpr uint32_t kTrapFrameOffset = 0x1800;
inline constexpr uint32_t kAbiMxcsr = 0x1F80;
inline constexpr uint16_t kAbiFpcw = 0x027F;

struct HostPart {
    uint64_t host_rsp;        // the host stack is free below this while guest code runs
    uint32_t guest_mxcsr;
    uint16_t guest_fpcw;
    uint16_t host_fpcw;
    uint32_t host_mxcsr;
    uint32_t depth;
    uint64_t base_rsp;        // the outermost enter64 frame; exit() and a fatal trap unwind to it
    GuestThread *thread;
    uint32_t guest_esp;       // the guest esp at the innermost host call or trap; a nested call() builds below it
    uint32_t index;
    uint32_t stack_base, stack_limit;   // the allocation; free_stack takes them back
    uint32_t stack_top;                 // where guest esp starts: below the TLS block when the thread has one
    uint32_t exit_code;
    uint32_t exited;
    void *host_thread;
    uint32_t host_thread_id;
    uint32_t in_trap;         // the landing is running: a second fault there is a landing bug
    SpawnArgs *spawn;
    uint32_t suspended;
};

inline constexpr int32_t kHostRsp = kHostPartOffset + offsetof(HostPart, host_rsp);
inline constexpr int32_t kGuestMxcsr = kHostPartOffset + offsetof(HostPart, guest_mxcsr);
inline constexpr int32_t kGuestFpcw = kHostPartOffset + offsetof(HostPart, guest_fpcw);
inline constexpr int32_t kHostFpcw = kHostPartOffset + offsetof(HostPart, host_fpcw);
inline constexpr int32_t kHostMxcsr = kHostPartOffset + offsetof(HostPart, host_mxcsr);
inline constexpr int32_t kBaseRsp = kHostPartOffset + offsetof(HostPart, base_rsp);
static_assert(kHostRsp == 0x1000 && kGuestMxcsr == 0x1008 && kGuestFpcw == 0x100C && kHostFpcw == 0x100E && kHostMxcsr == 0x1010);
static_assert(kTrapFrameOffset % 16 == 0, "the CONTEXT copy must be 16-aligned for fxrstor");

struct EntryState {
    uint32_t esp, eip, ecx, edx;
};

namespace ctx {
inline constexpr int32_t MxCsr = 0x34, EFlags = 0x44, Rax = 0x78, Rcx = 0x80, Rdx = 0x88, Rbx = 0x90, Rsp = 0x98,
                         Rbp = 0xA0, Rsi = 0xA8, Rdi = 0xB0, R15 = 0xF0, Rip = 0xF8, FltSave = 0x100;
}

}  // namespace xbrt::detail
