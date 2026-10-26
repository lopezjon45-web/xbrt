#include "core/devices.h"
#include "host/cpu/cpu_internal.h"

#include <Zydis/Zydis.h>

#include <intrin.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace xbrt::detail {

namespace {

constexpr uint32_t kPageTablesBase = 0xC0000000, kPageTablesEnd = 0xC0400000;
constexpr uint32_t kKernelImageBase = 0x80010000, kKernelImageEnd = 0x80040000;
constexpr uint32_t kFsWindow = 0x1000;   // fs base 0: a missed fs access faults below here, inside KPCR page 0
constexpr uint32_t kDfFlag = 0x400;
constexpr uint32_t kMaxRepCount = 1u << 20;

uint8_t *host(uint32_t va) { return reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(va)); }

std::string format(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return buf;
}

bool readable(uint64_t addr, size_t n)
{
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<void *>(addr), &info, sizeof info)) return false;
    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return addr + n <= reinterpret_cast<uint64_t>(info.BaseAddress) + info.RegionSize;
}

constexpr size_t kGprOffset[8] = {offsetof(CONTEXT, Rax), offsetof(CONTEXT, Rcx), offsetof(CONTEXT, Rdx), offsetof(CONTEXT, Rbx),
                                  offsetof(CONTEXT, Rsp), offsetof(CONTEXT, Rbp), offsetof(CONTEXT, Rsi), offsetof(CONTEXT, Rdi)};

uint32_t &gpr(CONTEXT &c, unsigned index) { return *reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(&c) + kGprOffset[index & 7]); }

struct Trap {
    ThreadBlock *tb;
    CONTEXT &c;
    const EXCEPTION_RECORD &rec;
    uint32_t eip, code;
    uint64_t addr;
    bool write;
    ZydisDecodedInstruction in;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    const uint8_t *bytes;

    uint32_t next() const { return eip + in.length; }
    uint32_t reg(unsigned i) { return gpr(c, i); }
};

std::string disasm(const Trap &t)
{
    ZydisFormatter f;
    ZydisFormatterInit(&f, ZYDIS_FORMATTER_STYLE_INTEL);
    char text[96];
    if (!ZYAN_SUCCESS(ZydisFormatterFormatInstruction(&f, &t.in, t.ops, t.in.operand_count_visible, text, sizeof text, t.eip, nullptr)))
        return "?";
    return text;
}

uint32_t read_reg(CONTEXT &c, ZydisRegister r)
{
    unsigned id = unsigned(ZydisRegisterGetId(r));
    switch (ZydisRegisterGetClass(r)) {
    case ZYDIS_REGCLASS_GPR32: return gpr(c, id);
    case ZYDIS_REGCLASS_GPR16: return gpr(c, id) & 0xFFFF;
    case ZYDIS_REGCLASS_GPR8: return id < 4 ? gpr(c, id) & 0xFF : (gpr(c, id - 4) >> 8) & 0xFF;
    default: return 0;
    }
}

bool write_reg(CONTEXT &c, ZydisRegister r, uint32_t v)
{
    unsigned id = unsigned(ZydisRegisterGetId(r));
    switch (ZydisRegisterGetClass(r)) {
    case ZYDIS_REGCLASS_GPR32: gpr(c, id) = v; return true;
    case ZYDIS_REGCLASS_GPR16: gpr(c, id) = (gpr(c, id) & 0xFFFF0000) | (v & 0xFFFF); return true;
    case ZYDIS_REGCLASS_GPR8:
        if (id < 4) gpr(c, id) = (gpr(c, id) & 0xFFFFFF00) | (v & 0xFF);
        else gpr(c, id - 4) = (gpr(c, id - 4) & 0xFFFF00FF) | ((v & 0xFF) << 8);
        return true;
    default: return false;
    }
}

uint32_t effective_address(Trap &t, const ZydisDecodedOperand &op)
{
    uint32_t ea = uint32_t(op.mem.disp.value);
    if (op.mem.base != ZYDIS_REGISTER_NONE) ea += read_reg(t.c, op.mem.base);
    if (op.mem.index != ZYDIS_REGISTER_NONE) ea += read_reg(t.c, op.mem.index) * op.mem.scale;
    return ea;
}

const MmioMap *mmio_at(uint32_t addr)
{
    std::lock_guard<std::mutex> hold(g_cpu->routes_lock);
    for (const MmioMap &m : g_cpu->mmio)
        if (addr >= m.base && addr < m.end) return &m;
    return nullptr;
}

const PortMap *port_at(uint16_t port)
{
    std::lock_guard<std::mutex> hold(g_cpu->routes_lock);
    for (const PortMap &p : g_cpu->ports)
        if (port >= p.first && port < p.end) return &p;
    return nullptr;
}

bool rdtsc_site(uint32_t eip)
{
    std::lock_guard<std::mutex> hold(g_cpu->routes_lock);
    return g_cpu->rdtsc_sites.count(eip) != 0;
}

struct DeviceScope {
    ThreadBlock *tb;
    explicit DeviceScope(ThreadBlock *b) : tb(b) { tb->host.mmio_in_progress++; }
    ~DeviceScope() { tb->host.mmio_in_progress--; }
    DeviceScope(const DeviceScope &) = delete;
    DeviceScope &operator=(const DeviceScope &) = delete;
};

struct Access {
    Trap &t;
    bool fs;
    uint32_t read(uint32_t addr, unsigned size)
    {
        if (fs && addr < kFsWindow) return load(reinterpret_cast<uint8_t *>(t.tb) + addr, size);
        if (const MmioMap *m = mmio_at(addr)) {
            DeviceScope scope(t.tb);
            return m->device->read({addr}, size);
        }
        if (!readable(addr, size)) fatal_trap(t.tb, format("read of unmapped 0x%08X at '%s'", addr, disasm(t).c_str()));
        return load(host(addr), size);
    }
    void write(uint32_t addr, unsigned size, uint32_t v)
    {
        if (fs && addr < kFsWindow) return store(reinterpret_cast<uint8_t *>(t.tb) + addr, size, v);
        if (const MmioMap *m = mmio_at(addr)) {
            DeviceScope scope(t.tb);
            return m->device->write({addr}, size, v);
        }
        if (!readable(addr, size)) fatal_trap(t.tb, format("write to unmapped 0x%08X at '%s'", addr, disasm(t).c_str()));
        store(host(addr), size, v);
    }
    static uint32_t load(const uint8_t *p, unsigned size)
    {
        uint32_t v = 0;
        memcpy(&v, p, size);
        return v;
    }
    static void store(uint8_t *p, unsigned size, uint32_t v) { memcpy(p, &v, size); }
};

uint32_t operand_read(Trap &t, Access &mem, const ZydisDecodedOperand &op)
{
    switch (op.type) {
    case ZYDIS_OPERAND_TYPE_REGISTER: return read_reg(t.c, op.reg.value);
    case ZYDIS_OPERAND_TYPE_MEMORY: return mem.read(effective_address(t, op), op.size / 8);
    case ZYDIS_OPERAND_TYPE_IMMEDIATE: return uint32_t(op.imm.value.u);
    default: fatal_trap(t.tb, format("operand kind %d at '%s'", int(op.type), disasm(t).c_str()));
    }
}

void operand_write(Trap &t, Access &mem, const ZydisDecodedOperand &op, uint32_t v)
{
    if (op.type == ZYDIS_OPERAND_TYPE_REGISTER) {
        if (!write_reg(t.c, op.reg.value, v)) fatal_trap(t.tb, format("cannot write register at '%s'", disasm(t).c_str()));
    } else if (op.type == ZYDIS_OPERAND_TYPE_MEMORY) {
        mem.write(effective_address(t, op), op.size / 8, v);
    } else {
        fatal_trap(t.tb, format("write to operand kind %d at '%s'", int(op.type), disasm(t).c_str()));
    }
}

enum Flags : uint32_t { CF = 1, PF = 4, AF = 0x10, ZF = 0x40, SF = 0x80, OF = 0x800 };

uint32_t mask_of(unsigned size) { return size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu; }

void set_result_flags(CONTEXT &c, unsigned size, uint32_t result, uint32_t keep)
{
    uint32_t m = mask_of(size), r = result & m, sign = 1u << (size * 8 - 1);
    uint32_t f = (c.EFlags & ~(CF | PF | AF | ZF | SF | OF)) | keep;
    if (r == 0) f |= ZF;
    if (r & sign) f |= SF;
    uint32_t parity = r & 0xFF;
    parity ^= parity >> 4;
    parity ^= parity >> 2;
    parity ^= parity >> 1;
    if (!(parity & 1)) f |= PF;
    c.EFlags = f;
}

void flags_add(CONTEXT &c, unsigned size, uint32_t a, uint32_t b, uint32_t r)
{
    uint32_t m = mask_of(size), sign = 1u << (size * 8 - 1), keep = 0;
    if ((r & m) < (a & m)) keep |= CF;
    if (((a ^ r) & (b ^ r)) & sign) keep |= OF;
    if (((a ^ b ^ r) & 0x10)) keep |= AF;
    set_result_flags(c, size, r, keep);
}

void flags_sub(CONTEXT &c, unsigned size, uint32_t a, uint32_t b, uint32_t r)
{
    uint32_t m = mask_of(size), sign = 1u << (size * 8 - 1), keep = 0;
    if ((a & m) < (b & m)) keep |= CF;
    if (((a ^ b) & (a ^ r)) & sign) keep |= OF;
    if (((a ^ b ^ r) & 0x10)) keep |= AF;
    set_result_flags(c, size, r, keep);
}

void guest_push(Trap &t, unsigned size, uint32_t v)
{
    uint32_t esp = t.reg(4) - size;
    if (!readable(esp, size)) fatal_trap(t.tb, format("push below the guest stack at 0x%08X", esp));
    Access::store(host(esp), size, v);
    gpr(t.c, 4) = esp;
}

uint32_t guest_pop(Trap &t, unsigned size)
{
    uint32_t esp = t.reg(4);
    if (!readable(esp, size)) fatal_trap(t.tb, format("pop above the guest stack at 0x%08X", esp));
    uint32_t v = Access::load(host(esp), size);
    gpr(t.c, 4) = esp + size;
    return v;
}

void emulate(Trap &t, bool fs)
{
    Access mem{t, fs};
    CONTEXT &c = t.c;
    const ZydisDecodedOperand *op = t.ops;
    unsigned size = op[0].size / 8;
    uint32_t a, b, r;
    switch (t.in.mnemonic) {
    case ZYDIS_MNEMONIC_MOV: operand_write(t, mem, op[0], operand_read(t, mem, op[1])); return;
    case ZYDIS_MNEMONIC_MOVZX: operand_write(t, mem, op[0], operand_read(t, mem, op[1])); return;
    case ZYDIS_MNEMONIC_MOVSX: {
        unsigned from = op[1].size / 8;
        uint32_t v = operand_read(t, mem, op[1]);
        if (from == 1) v = uint32_t(int32_t(int8_t(v)));
        else if (from == 2) v = uint32_t(int32_t(int16_t(v)));
        operand_write(t, mem, op[0], v);
        return;
    }
    case ZYDIS_MNEMONIC_PUSH: guest_push(t, size, operand_read(t, mem, op[0])); return;
    case ZYDIS_MNEMONIC_POP: {
        uint32_t v = guest_pop(t, size);
        operand_write(t, mem, op[0], v);
        return;
    }
    case ZYDIS_MNEMONIC_XCHG: {
        a = operand_read(t, mem, op[0]);
        b = operand_read(t, mem, op[1]);
        operand_write(t, mem, op[0], b);
        operand_write(t, mem, op[1], a);
        return;
    }
    case ZYDIS_MNEMONIC_CMP:
        a = operand_read(t, mem, op[0]);
        b = operand_read(t, mem, op[1]);
        flags_sub(c, size, a, b, a - b);
        return;
    case ZYDIS_MNEMONIC_TEST:
        a = operand_read(t, mem, op[0]);
        b = operand_read(t, mem, op[1]);
        set_result_flags(c, size, a & b, 0);
        return;
    case ZYDIS_MNEMONIC_ADD:
    case ZYDIS_MNEMONIC_SUB:
    case ZYDIS_MNEMONIC_AND:
    case ZYDIS_MNEMONIC_OR:
    case ZYDIS_MNEMONIC_XOR:
        a = operand_read(t, mem, op[0]);
        b = operand_read(t, mem, op[1]);
        switch (t.in.mnemonic) {
        case ZYDIS_MNEMONIC_ADD: r = a + b; flags_add(c, size, a, b, r); break;
        case ZYDIS_MNEMONIC_SUB: r = a - b; flags_sub(c, size, a, b, r); break;
        case ZYDIS_MNEMONIC_AND: r = a & b; set_result_flags(c, size, r, 0); break;
        case ZYDIS_MNEMONIC_OR: r = a | b; set_result_flags(c, size, r, 0); break;
        default: r = a ^ b; set_result_flags(c, size, r, 0); break;
        }
        operand_write(t, mem, op[0], r);
        return;
    case ZYDIS_MNEMONIC_INC:
    case ZYDIS_MNEMONIC_DEC: {
        a = operand_read(t, mem, op[0]);
        uint32_t cf = c.EFlags & CF;
        r = t.in.mnemonic == ZYDIS_MNEMONIC_INC ? a + 1 : a - 1;
        if (t.in.mnemonic == ZYDIS_MNEMONIC_INC) flags_add(c, size, a, 1, r);
        else flags_sub(c, size, a, 1, r);
        c.EFlags = (c.EFlags & ~CF) | cf;   // inc and dec leave CF alone
        operand_write(t, mem, op[0], r);
        return;
    }
    case ZYDIS_MNEMONIC_NOT: operand_write(t, mem, op[0], ~operand_read(t, mem, op[0])); return;
    case ZYDIS_MNEMONIC_NEG:
        a = operand_read(t, mem, op[0]);
        r = 0u - a;
        flags_sub(c, size, 0, a, r);
        operand_write(t, mem, op[0], r);
        return;
    case ZYDIS_MNEMONIC_STOSB:
    case ZYDIS_MNEMONIC_STOSW:
    case ZYDIS_MNEMONIC_STOSD:
    case ZYDIS_MNEMONIC_MOVSB:
    case ZYDIS_MNEMONIC_MOVSW:
    case ZYDIS_MNEMONIC_MOVSD: {
        // The string forms have two memory operands (es:[edi], ds:[esi]); the SSE movsd shares the mnemonic.
        bool movs = op[1].type == ZYDIS_OPERAND_TYPE_MEMORY;
        bool stos = op[1].type == ZYDIS_OPERAND_TYPE_REGISTER && op[0].type == ZYDIS_OPERAND_TYPE_MEMORY && op[0].mem.base == ZYDIS_REGISTER_EDI;
        if (!movs && !stos) fatal_trap(t.tb, format("cannot emulate '%s'", disasm(t).c_str()));
        bool rep = (t.in.attributes & ZYDIS_ATTRIB_HAS_REP) != 0;
        uint32_t count = rep ? t.reg(1) : 1;
        if (count > kMaxRepCount) fatal_trap(t.tb, format("rep count 0x%X at '%s'", count, disasm(t).c_str()));
        int32_t step = (c.EFlags & kDfFlag) ? -int32_t(size) : int32_t(size);
        for (uint32_t i = 0; i < count; i++) {
            uint32_t v = movs ? mem.read(t.reg(6), size) : t.reg(0);
            mem.write(t.reg(7), size, v);
            gpr(c, 7) += uint32_t(step);
            if (movs) gpr(c, 6) += uint32_t(step);
        }
        if (rep) gpr(c, 1) = 0;
        return;
    }
    default: fatal_trap(t.tb, format("cannot emulate '%s'", disasm(t).c_str()));
    }
}

void emulate_port(Trap &t)
{
    CONTEXT &c = t.c;
    const ZydisDecodedOperand *op = t.ops;
    bool out = t.in.mnemonic == ZYDIS_MNEMONIC_OUT || t.in.mnemonic == ZYDIS_MNEMONIC_OUTSB ||
               t.in.mnemonic == ZYDIS_MNEMONIC_OUTSW || t.in.mnemonic == ZYDIS_MNEMONIC_OUTSD;
    bool string = t.in.mnemonic != ZYDIS_MNEMONIC_IN && t.in.mnemonic != ZYDIS_MNEMONIC_OUT;
    const ZydisDecodedOperand &port_op = out ? op[0] : op[1];
    const ZydisDecodedOperand &data_op = out ? op[1] : op[0];
    uint16_t port = uint16_t(port_op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE ? port_op.imm.value.u : t.reg(2) & 0xFFFF);
    unsigned size = data_op.size / 8;
    const PortMap *dev = port_at(port);
    if (!dev) fatal_trap(t.tb, format("cannot emulate '%s' (port 0x%X)", disasm(t).c_str(), port));
    Access mem{t, false};
    bool rep = (t.in.attributes & ZYDIS_ATTRIB_HAS_REP) != 0;
    uint32_t count = string && rep ? t.reg(1) : 1;
    if (count > kMaxRepCount) fatal_trap(t.tb, format("rep count 0x%X at '%s'", count, disasm(t).c_str()));
    int32_t step = (c.EFlags & kDfFlag) ? -int32_t(size) : int32_t(size);
    for (uint32_t i = 0; i < count; i++) {
        if (out) {
            uint32_t v = string ? mem.read(t.reg(6), size) : read_reg(c, data_op.reg.value);
            dev->device->out(port, size, v);
            if (string) gpr(c, 6) += uint32_t(step);
        } else {
            uint32_t v = dev->device->in(port, size);
            if (string) {
                mem.write(t.reg(7), size, v);
                gpr(c, 7) += uint32_t(step);
            } else {
                write_reg(c, data_op.reg.value, v);
            }
        }
    }
    if (string && rep) gpr(c, 1) = 0;
}

// The model-specific registers a title may read, as the console's Pentium III answers them. EBL_CR_POWERON's bits
// 22-27 carry the bus-to-core ratio, 5.5 on a retail console (733 MHz on a 133 MHz bus), which a runtime reads at
// start to know its TSC rate.
bool read_msr(uint32_t msr, uint64_t &value)
{
    constexpr uint32_t kTsc = 0x10, kEblCrPoweron = 0x2A;
    constexpr uint64_t kRatio5_5 = 0b000100;
    switch (msr) {
    case kTsc: value = guest_tsc_now(); return true;
    case kEblCrPoweron: value = kRatio5_5 << 22; return true;
    default: return false;
    }
}

bool has_fs_operand(const Trap &t)
{
    for (unsigned i = 0; i < t.in.operand_count; i++)
        if (t.ops[i].type == ZYDIS_OPERAND_TYPE_MEMORY && t.ops[i].mem.segment == ZYDIS_REGISTER_FS) return true;
    return false;
}

bool is_segment_load(const Trap &t)
{
    switch (t.in.mnemonic) {
    case ZYDIS_MNEMONIC_LDS:
    case ZYDIS_MNEMONIC_LES:
    case ZYDIS_MNEMONIC_LFS:
    case ZYDIS_MNEMONIC_LGS:
    case ZYDIS_MNEMONIC_LSS: return true;
    case ZYDIS_MNEMONIC_MOV:
    case ZYDIS_MNEMONIC_POP:
        return t.ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER && ZydisRegisterGetClass(t.ops[0].reg.value) == ZYDIS_REGCLASS_SEGMENT;
    default: return false;
    }
}

void trace(const Trap &t, const char *row, uint32_t next)
{
    if (!g_cpu->options.trace_traps) return;
    char line[160];
    snprintf(line, sizeof line, "xbrt trap %s thread %u eip 0x%08X '%s' -> 0x%08X\n", row, t.tb->host.index, t.eip,
             disasm(t).c_str(), next);
    OutputDebugStringA(line);
}

void promote_from_trap(Trap &t, SiteKind privileged_or_fs)
{
    PatchSite site{};
    site.at = {t.eip};
    site.length = t.in.length;
    site.kind = privileged_or_fs;
    if (privileged_or_fs != SiteKind::Privileged) {
        FsForm form = classify_fs({t.bytes, t.in.length});
        site.kind = form.kind;
        site.reg = form.reg;
        site.disp = form.disp;
        site.imm = form.imm;
    }
    memcpy(site.original.data(), t.bytes, t.in.length);
    if (!promote(site, true)) g_cpu->fs_unpromotable += privileged_or_fs == SiteKind::Privileged ? 0 : 1;
}

// ---- Exception delivery: a guest fault or raise reaches the title's fs:[0] handlers ----------------------------------

constexpr uint32_t kStatusAccessViolation = 0xC0000005;
constexpr uint16_t kXboxCs = 0x08, kXboxSs = 0x10;   // the console runs titles in ring 0

// The stop to make for each exception this thread is dispatching, should no handler take it, by the esp its
// dispatcher started at; a filter can raise and handle an exception of its own while an outer one is being dispatched.
struct Undelivered {
    uint32_t esp;
    CONTEXT ctx;
    std::string what;
};
thread_local std::vector<Undelivered> t_undelivered;

void note_undelivered(uint32_t esp, const CONTEXT &c, std::string what)
{
    // A dispatch whose frames lie below the stack pointer now has ended.
    std::erase_if(t_undelivered, [&](const Undelivered &u) { return u.esp < uint32_t(c.Rsp); });
    t_undelivered.push_back({esp, c, std::move(what)});
}

// The record, the CONTEXT (FloatSave 16-aligned for fxrstor) and the dispatcher's four arguments, built below `esp`
// with a gap the dispatcher's resume uses. Returns the dispatcher's esp, or 0 when the thread's stack has no room.
uint32_t push_exception(ThreadBlock *tb, uint32_t esp, const XboxContext &context, const XboxExceptionRecord &record)
{
    uint32_t context_va = ((esp - 0x20 - uint32_t(sizeof(XboxContext))) & ~15u) - 4;
    uint32_t record_va = context_va - uint32_t(sizeof(XboxExceptionRecord));
    uint32_t args_va = record_va - 16;
    if (args_va > esp || !readable(args_va, esp - args_va)) return 0;
    memcpy(host(context_va), &context, sizeof context);
    memcpy(host(record_va), &record, sizeof record);
    uint32_t bound = tb->host.call_esp ? tb->host.call_esp : tb->kpcr.NtTib.StackBase;
    uint32_t args[4] = {record_va, context_va, uint32_t(reinterpret_cast<uintptr_t>(tb)), bound};
    memcpy(host(args_va), args, sizeof args);
    return args_va;
}

// The fault in `t` goes to the title's handlers: the guest resumes in the dispatcher. `what` is the stop if none takes
// it.
uint32_t deliver(Trap &t, const XboxExceptionRecord &record, std::string what)
{
    CONTEXT &c = t.c;
    XboxContext x{};
    x.ContextFlags = kXboxContextFull;
    memcpy(x.FloatSave, &c.FltSave, sizeof x.FloatSave);
    x.Edi = uint32_t(c.Rdi), x.Esi = uint32_t(c.Rsi), x.Ebx = uint32_t(c.Rbx), x.Edx = uint32_t(c.Rdx);
    x.Ecx = uint32_t(c.Rcx), x.Eax = uint32_t(c.Rax), x.Ebp = uint32_t(c.Rbp), x.Eip = t.eip;
    x.SegCs = kXboxCs, x.EFlags = c.EFlags, x.Esp = uint32_t(c.Rsp), x.SegSs = kXboxSs;
    uint32_t esp = push_exception(t.tb, uint32_t(c.Rsp), x, record);
    if (!esp) fatal_trap(t.tb, what + "; the guest stack has no room to deliver it to the title's handlers");
    g_cpu->traps_delivered++;
    trace(t, "delivered", g_cpu->gates.seh_dispatch);
    note_undelivered(esp, c, std::move(what));
    c.Rsp = esp;
    c.EFlags &= ~kDfFlag;
    // A pending x87 exception stays in the title's CONTEXT; the dispatcher's own x87 use must not raise it again.
    c.FltSave.StatusWord &= 0x7F00;
    return g_cpu->gates.seh_dispatch;
}

[[noreturn]] void undelivered(ThreadBlock *tb, uint32_t esp)
{
    auto it = std::find_if(t_undelivered.rbegin(), t_undelivered.rend(), [&](const Undelivered &u) { return u.esp == esp; });
    if (it == t_undelivered.rend()) fatal_trap(tb, "the exception dispatcher reached its end with no exception pending");
    tb->trap.ctx = it->ctx;
    fatal_trap(tb, it->what + " (no handler on the fs:[0] chain took it)");
}

uint32_t route(Trap &t)
{
    CpuState &s = *g_cpu;
    if (t.c.SegCs == kLongCs)   // only a gate can fault in 64-bit mode on a guest thread
        fatal_trap(t.tb, format("fault 0x%08X at 0x%llX inside the gates", t.code, static_cast<unsigned long long>(t.c.Rip)));
    if (t.eip == s.gates.seh_unhandled) undelivered(t.tb, uint32_t(t.c.Rsp));
    if (t.code == STATUS_PRIVILEGED_INSTRUCTION && t.eip == s.patching_site.load()) {
        s.traps_patch_wait++;
        SwitchToThread();
        return t.eip;
    }
    {
        // A site another thread promoted between this thread's fault and its landing now holds a far call that
        // does not fault: run it in place instead of decoding bytes that are not the ones that trapped.
        std::lock_guard<std::mutex> hold(s.patch_lock);
        if (s.promoted.count(t.eip)) {
            s.traps_patch_wait++;
            return t.eip;
        }
    }
    bool fetch = t.code == STATUS_ACCESS_VIOLATION && t.rec.NumberParameters > 0 && t.rec.ExceptionInformation[0] == 8;
    if (fetch || !readable(t.eip, 1)) fatal_trap(t.tb, format("fault fetching code at 0x%08X", t.eip));
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
    size_t avail = std::min<size_t>(15, 0x1000 - (t.eip & 0xFFF));
    if (avail < 15 && readable(t.eip + avail, 15 - avail)) avail = 15;
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, t.bytes, avail, &t.in, t.ops)))
        fatal_trap(t.tb, format("cannot decode the bytes at 0x%08X", t.eip));

    if (t.code == STATUS_ACCESS_VIOLATION) {
        if (t.addr < kFsWindow && has_fs_operand(t)) {
            s.traps_fs++;
            emulate(t, true);
            promote_from_trap(t, SiteKind::FsOther);
            trace(t, "fs", t.next());
            return t.next();
        }
        if (mmio_at(uint32_t(t.addr))) {
            s.traps_mmio++;
            emulate(t, false);
            trace(t, "mmio", t.next());
            return t.next();
        }
        if (t.addr >= kPageTablesBase && t.addr < kPageTablesEnd)
            fatal_trap(t.tb, format("fault on 0x%08X (page tables) at '%s'", uint32_t(t.addr), disasm(t).c_str()));
        if (t.addr >= kKernelImageBase && t.addr < kKernelImageEnd)
            fatal_trap(t.tb, format("fault on 0x%08X (kernel image) at '%s'", uint32_t(t.addr), disasm(t).c_str()));
        if (is_segment_load(t)) fatal_trap(t.tb, format("selector load '%s'", disasm(t).c_str()));
        // A general protection fault (no address): an instruction ring 3 refuses that the console's ring 0 runs.
        if (t.addr == ~0ull) fatal_trap(t.tb, format("protection fault at '%s'", disasm(t).c_str()));
        if (const DeviceAperture *d = device_aperture_at(uint32_t(t.addr)))
            fatal_trap(t.tb, format("%s register 0x%08X %s: not modelled ('%s')", std::string(d->name).c_str(), uint32_t(t.addr),
                                    t.write ? "write" : "read", disasm(t).c_str()),
                       true);
        XboxExceptionRecord record{kStatusAccessViolation, 0, 0, t.eip, 2, {t.write ? 1u : 0u, uint32_t(t.addr)}};
        return deliver(t, record,
                       format("%s fault on 0x%08X at '%s' (ss=0x%X cs=0x%X)", t.write ? "write" : "read", uint32_t(t.addr),
                              disasm(t).c_str(), t.c.SegSs, t.c.SegCs));
    }
    if (t.code == STATUS_PRIVILEGED_INSTRUCTION) {
        if (t.bytes[0] == kRdtscTrap[0] && rdtsc_site(t.eip)) {
            uint64_t tsc = guest_tsc_now();
            gpr(t.c, 0) = uint32_t(tsc);
            gpr(t.c, 2) = uint32_t(tsc >> 32);
            s.traps_rdtsc++;
            return t.eip + 2;
        }
        switch (t.in.mnemonic) {
        case ZYDIS_MNEMONIC_IN:
        case ZYDIS_MNEMONIC_OUT:
        case ZYDIS_MNEMONIC_INSB:
        case ZYDIS_MNEMONIC_INSW:
        case ZYDIS_MNEMONIC_INSD:
        case ZYDIS_MNEMONIC_OUTSB:
        case ZYDIS_MNEMONIC_OUTSW:
        case ZYDIS_MNEMONIC_OUTSD:
            s.traps_port++;
            emulate_port(t);
            trace(t, "port", t.next());
            return t.next();
        case ZYDIS_MNEMONIC_WBINVD:
        case ZYDIS_MNEMONIC_INVD:
        case ZYDIS_MNEMONIC_CLI:
        case ZYDIS_MNEMONIC_STI:
            s.traps_privileged++;
            promote_from_trap(t, SiteKind::Privileged);
            trace(t, "privileged", t.next());
            return t.next();
        case ZYDIS_MNEMONIC_HLT:
            s.traps_privileged++;
            SwitchToThread();
            return t.next();
        case ZYDIS_MNEMONIC_RDMSR: {
            uint64_t v = 0;
            if (!read_msr(t.reg(1), v))
                fatal_trap(t.tb, format("cannot emulate '%s' (MSR 0x%X)", disasm(t).c_str(), t.reg(1)));
            gpr(t.c, 0) = uint32_t(v);
            gpr(t.c, 2) = uint32_t(v >> 32);
            s.traps_privileged++;
            trace(t, "msr", t.next());
            return t.next();
        }
        default: break;
        }
        if (is_segment_load(t)) fatal_trap(t.tb, format("selector load '%s'", disasm(t).c_str()));
        fatal_trap(t.tb, format("cannot emulate '%s'", disasm(t).c_str()));
    }
    if (t.code == STATUS_BREAKPOINT) {
        const uint8_t *p = t.bytes;
        uint32_t at = t.eip;
        if (!(p[0] == 0xCD && p[1] == 0x2D) && t.eip >= 2 && readable(t.eip - 2, 2) && p[-2] == 0xCD && p[-1] == 0x2D) at = t.eip - 2;
        if (host(at)[0] == 0xCD && host(at)[1] == 0x2D) {
            if (t.reg(0) == 1 && readable(t.reg(1), 8)) {
                const uint8_t *ansi = host(t.reg(1));
                uint16_t length = Access::load(ansi, 2) & 0xFFFF;
                uint32_t buffer = Access::load(ansi + 4, 4);
                if (readable(buffer, length)) OutputDebugStringA(std::string(reinterpret_cast<const char *>(host(buffer)), length).c_str());
            }
            s.traps_privileged++;
            return at + 2;
        }
        fatal_trap(t.tb, format("breakpoint at 0x%08X ('%s')", t.eip, disasm(t).c_str()));
    }
    XboxExceptionRecord record{t.code, 0, 0, t.eip, 0, {}};
    return deliver(t, record, format("exception 0x%08X at '%s'", t.code, disasm(t).c_str()));
}

}  // namespace

}  // namespace xbrt::detail

namespace xbrt {

bool Cpu::raise(GuestThread &thread, const GateFrame &frame, GuestVa record, uint32_t &esp, uint32_t &eip)
{
    using namespace detail;
    ThreadBlock *tb = block_of(thread);
    if (!tb || !readable(frame.esp, 8) || !readable(record.v, sizeof(XboxExceptionRecord))) return false;
    XboxExceptionRecord r;
    memcpy(&r, host(record.v), sizeof r);
    uint32_t caller = Access::load(host(frame.esp), 4);
    r.ExceptionAddress = caller;
    if (r.NumberParameters > 15) r.NumberParameters = 15;
    // The CONTEXT RtlRaiseException captures: its caller's registers, as if it had returned (stdcall, one argument).
    alignas(16) uint8_t fx[512];
    _fxsave(fx);
    memcpy(fx, &frame.fpcw, 2);
    memcpy(fx + 24, &frame.mxcsr, 4);
    XboxContext x{};
    x.ContextFlags = kXboxContextFull;
    memcpy(x.FloatSave, fx, sizeof x.FloatSave);
    x.Edi = frame.edi, x.Esi = frame.esi, x.Ebx = frame.ebx, x.Edx = frame.edx, x.Ecx = frame.ecx, x.Eax = frame.eax;
    x.Ebp = frame.ebp, x.Eip = caller, x.SegCs = kXboxCs, x.EFlags = 0x202, x.Esp = frame.esp + 8, x.SegSs = kXboxSs;
    esp = push_exception(tb, frame.esp, x, r);
    if (!esp) return false;
    CONTEXT c{};
    c.Rip = caller, c.Rsp = x.Esp, c.Rax = frame.eax, c.Rcx = frame.ecx, c.Rdx = frame.edx, c.Rbx = frame.ebx;
    c.Rbp = frame.ebp, c.Rsi = frame.esi, c.Rdi = frame.edi, c.EFlags = x.EFlags, c.SegCs = kCompatCs, c.SegSs = kFlatData;
    note_undelivered(esp, c, format("exception 0x%08X raised by the title at 0x%08X", r.ExceptionCode, caller));
    eip = g_cpu->gates.seh_dispatch;
    return true;
}

}  // namespace xbrt

namespace xbrt::detail {

[[noreturn]] void fatal_trap(ThreadBlock *tb, const std::string &what, bool device_register)
{
    CONTEXT &c = tb->trap.ctx;
    TrapReport r{};
    r.what = what;
    r.device_register = device_register;
    r.where = {uint32_t(c.Rip)};
    r.thread = tb->host.index;
    if (readable(c.Rip, r.bytes_at_eip.size())) memcpy(r.bytes_at_eip.data(), host(uint32_t(c.Rip)), r.bytes_at_eip.size());
    if (readable(c.Rsp, sizeof r.stack_at_esp)) memcpy(r.stack_at_esp.data(), host(uint32_t(c.Rsp)), sizeof r.stack_at_esp);
    r.eax = uint32_t(c.Rax);
    r.ecx = uint32_t(c.Rcx);
    r.edx = uint32_t(c.Rdx);
    r.ebx = uint32_t(c.Rbx);
    r.esp = uint32_t(c.Rsp);
    r.ebp = uint32_t(c.Rbp);
    r.esi = uint32_t(c.Rsi);
    r.edi = uint32_t(c.Rdi);
    r.eflags = c.EFlags;
    r.exception_list = tb->kpcr.NtTib.ExceptionList;
    r.stack_base = tb->kpcr.NtTib.StackBase;
    r.stack_limit = tb->kpcr.NtTib.StackLimit;
    r.irql = tb->kpcr.Irql;
    r.current_thread = tb->kpcr.PrcbData.CurrentThread;
    r.tls_data = tb->ethread.Tcb.TlsData;
    if (r.tls_data && readable(r.tls_data, 4)) r.tls_slot0 = Access::load(host(r.tls_data), 4);
    uint32_t ebp = uint32_t(c.Rbp);
    for (int i = 0; i < 32 && ebp >= tb->host.stack_limit && ebp <= tb->host.stack_base - 8; i++) {
        r.ebp_chain.push_back({Access::load(host(ebp + 4), 4)});
        uint32_t next = Access::load(host(ebp), 4);
        if (next <= ebp) break;
        ebp = next;
    }
    g_cpu->on_fatal(r);
    unwind_thread(tb, kFatalExitCode);
}

// Runs on the guest stack in 64-bit mode (the kernel built the frame there), so it only copies and redirects.
LONG CALLBACK cpu_veh(EXCEPTION_POINTERS *e)
{
    CONTEXT *c = e->ContextRecord;
    ThreadBlock *tb = t_block;
    if (!tb || tb->host.in_trap || tb->host.depth == 0) return EXCEPTION_CONTINUE_SEARCH;
    bool guest = c->SegCs == kCompatCs, gate = c->SegCs == kLongCs && in_gate_region(c->Rip);
    if (!guest && !gate) return EXCEPTION_CONTINUE_SEARCH;
    _mm_setcsr(kAbiMxcsr);
    memcpy(&tb->trap.ctx, c, sizeof(CONTEXT));
    memcpy(&tb->trap.rec, e->ExceptionRecord, sizeof(EXCEPTION_RECORD));
    tb->host.in_trap = 1;
    uint64_t sp = (tb->host.host_rsp - 0x200) & ~uint64_t(0xF);
    c->Rip = reinterpret_cast<uint64_t>(&xbrt_trap_landing);
    c->Rcx = reinterpret_cast<uint64_t>(tb);
    c->Rsp = sp - 8;
    c->SegCs = kLongCs;
    c->SegSs = kFlatData;
    c->MxCsr = kAbiMxcsr;
    c->FltSave.MxCsr = kAbiMxcsr;
    c->FltSave.ControlWord = kAbiFpcw;
    c->EFlags &= ~kDfFlag;
    return EXCEPTION_CONTINUE_EXECUTION;
}

extern "C" [[noreturn]] void xbrt_trap_landing(ThreadBlock *tb)
{
    if (tb->host.depth == 1) tb->host.base_rsp = tb->host.host_rsp;
    CONTEXT &c = tb->trap.ctx;
    const EXCEPTION_RECORD &rec = tb->trap.rec;
    Trap t{tb, c, rec, uint32_t(c.Rip), rec.ExceptionCode,
           rec.NumberParameters > 1 ? rec.ExceptionInformation[1] : ~0ull,
           rec.NumberParameters > 0 && rec.ExceptionInformation[0] == 1, {}, {}, host(uint32_t(c.Rip))};
    tb->host.guest_esp = uint32_t(c.Rsp);
    tb->host.guest_mxcsr = c.MxCsr;
    tb->host.guest_fpcw = c.FltSave.ControlWord;
    uint32_t next = route(t);
    c.Rip = next;
    tb->host.in_trap = 0;
    reinterpret_cast<ResumeFn>(static_cast<uintptr_t>(g_cpu->gates.resume64))(&c);
    for (;;) {}
}

}  // namespace xbrt::detail
