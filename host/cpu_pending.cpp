// Link-only placeholders for the workstream-A definitions host/kernel.cpp calls (host/cpu.h, host/memory.h), so this
// branch links before A lands. Every one stops the process naming itself; none returns a value. CI on main fails
// while this file exists: whoever merges second deletes it (rebasing onto A's branch).
#include "host/cpu.h"
#include "host/memory.h"

#include <cstdio>
#include <cstdlib>

namespace xbrt {

namespace {

[[noreturn]] void not_built(const char *what)
{
    std::fprintf(stderr, "not built yet (U2 workstream A): %s\n", what);
    std::fflush(stderr);
    std::abort();
}

}  // namespace

// An identity with no state, so on_host_call has a thread to be handed; every member below stops.
GuestThread &GuestThread::current()
{
    static GuestThread placeholder;
    return placeholder;
}
GuestThread &GuestThread::spawn(GuestVa, std::span<const uint32_t>, const SpawnOptions &) { not_built("GuestThread::spawn"); }
uint64_t GuestThread::call(GuestVa, std::span<const uint32_t>, CallConv) { not_built("GuestThread::call"); }
void GuestThread::exit(uint32_t) { not_built("GuestThread::exit"); }
GuestVa GuestThread::kpcr() const { not_built("GuestThread::kpcr"); }
void GuestThread::resume() { not_built("GuestThread::resume"); }
uint32_t GuestThread::host_thread_id() const { not_built("GuestThread::host_thread_id"); }

uint64_t Cpu::guest_tsc() const { not_built("Cpu::guest_tsc"); }

uint32_t GuestMemory::allocate_virtual(GuestVa *, uint32_t *, uint32_t, uint32_t) { not_built("GuestMemory::allocate_virtual"); }
uint32_t GuestMemory::free_virtual(GuestVa *, uint32_t *, uint32_t) { not_built("GuestMemory::free_virtual"); }
uint32_t GuestMemory::protect_virtual(GuestVa *, uint32_t *, uint32_t, uint32_t *) { not_built("GuestMemory::protect_virtual"); }
uint32_t GuestMemory::query_virtual(GuestVa, XboxMemoryBasicInformation *) const { not_built("GuestMemory::query_virtual"); }
GuestVa GuestMemory::allocate_contiguous(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { not_built("GuestMemory::allocate_contiguous"); }
void GuestMemory::free_contiguous(GuestVa) { not_built("GuestMemory::free_contiguous"); }
GuestVa GuestMemory::allocate_system(uint32_t, uint32_t) { not_built("GuestMemory::allocate_system"); }
void GuestMemory::free_system(GuestVa, uint32_t) { not_built("GuestMemory::free_system"); }
uint32_t GuestMemory::physical(GuestVa) const { not_built("GuestMemory::physical"); }
uint32_t GuestMemory::allocation_size(GuestVa) const { not_built("GuestMemory::allocation_size"); }
GuestMemory::Statistics GuestMemory::statistics() const { not_built("GuestMemory::statistics"); }

}  // namespace xbrt
