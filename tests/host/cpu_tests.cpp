#include "tests/host/cpu_tests.h"

#include "host/cpu.h"
#include "host/cpu/cpu_internal.h"

#include <Zydis/Zydis.h>

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace xbrt {

namespace {

using detail::ThreadBlock;

std::string format(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return buf;
}

// 32-bit payloads, as a title's code would be. Each ends in `ret` to the exit32 address call() pushed.
struct P {
    std::vector<uint8_t> b;
    P &raw(std::initializer_list<uint8_t> l)
    {
        b.insert(b.end(), l);
        return *this;
    }
    P &u32(uint32_t v)
    {
        for (int i = 0; i < 4; i++) b.push_back(uint8_t(v >> (8 * i)));
        return *this;
    }
    P &ret() { return raw({0xC3}); }
    P &push_imm8(uint8_t v) { return raw({0x6A, v}); }
    P &push_imm32(uint32_t v) { return raw({0x68}).u32(v); }
    P &mov_eax_imm(uint32_t v) { return raw({0xB8}).u32(v); }
    P &mov_ecx_imm(uint32_t v) { return raw({0xB9}).u32(v); }
    P &mov_edx_imm(uint32_t v) { return raw({0xBA}).u32(v); }
    P &call_ecx() { return raw({0xFF, 0xD1}); }
    P &jmp_far_ptr(uint32_t ptr) { return raw({0xFF, 0x2D}).u32(ptr); }
};

struct Placed {
    GuestMemory *memory;
    uint32_t cursor, end;
    uint32_t put(const std::vector<uint8_t> &bytes)
    {
        uint32_t at = (cursor + 15) & ~15u;
        if (at + bytes.size() > end) return 0;
        memcpy(memory->host<uint8_t>({at}), bytes.data(), bytes.size());
        FlushInstructionCache(GetCurrentProcess(), memory->host<uint8_t>({at}), bytes.size());
        cursor = at + uint32_t(bytes.size());
        return at;
    }
};

using Handler = std::function<void(GuestThread &, GateFrame &)>;
std::map<uint16_t, Handler> g_handlers;
TrapReport g_fatal;
int g_fatals = 0;

void on_host_call(GuestThread &t, HostCallId id, GateFrame &f) noexcept
{
    auto it = g_handlers.find(id.raw());
    if (it != g_handlers.end()) {
        it->second(t, f);
        return;
    }
    f.eax = 0xBAD0BAD0;
    f.eip = *reinterpret_cast<uint32_t *>(static_cast<uintptr_t>(f.esp));
    f.esp += 4;
}

void on_fatal(const TrapReport &r) noexcept
{
    g_fatal = r;
    g_fatals++;
}

uint32_t arg(const GateFrame &f, int i) { return *reinterpret_cast<uint32_t *>(static_cast<uintptr_t>(f.esp + 4 + 4 * i)); }

void stdcall_return(GateFrame &f, unsigned args)
{
    f.eip = *reinterpret_cast<uint32_t *>(static_cast<uintptr_t>(f.esp));
    f.esp += 4 + 4 * args;
}

struct Result {
    bool ok;
    std::string detail;
};

struct Tests {
    GuestMemory &memory;
    Cpu &cpu;
    Placed code;
    CpuCounters before{};

    void snap() { before = cpu.counters(); }
    uint64_t run(uint32_t payload, std::initializer_list<uint32_t> args = {}, CallConv conv = CallConv::Stdcall)
    {
        std::vector<uint32_t> a(args);
        return GuestThread::current().call({payload}, a, conv);
    }
    detail::XboxKpcr &kpcr() { return *memory.host<detail::XboxKpcr>(GuestThread::current().kpcr()); }

    Result emit()
    {
        struct Case {
            const char *name;
            std::vector<uint8_t> bytes;
            bool long64;
            const char *expect;
        };
        const Case cases[] = {
            {"enter64", detail::emit_enter64(), true,
             "push push push push push push push push sub movdqa movdqa mov mov mov mov ldmxcsr fldcw mov mov mov mov mov mov "
             "mov lea mov mov mov mov mov xor xor xor xor xor retf"},
            {"exit32", detail::emit_exit32(0x90010000), false, "jmp-far"},
            {"exit64", detail::emit_exit64(), true,
             "mov stmxcsr fnstcw ldmxcsr fldcw cld mov shl or mov mov movdqa movdqa add pop pop pop pop pop pop pop pop ret"},
            {"full_gate64", detail::emit_full_gate64(0x7FF612345678ull), true,
             "mov mov lea mov mov mov mov mov mov mov mov mov mov stmxcsr fnstcw ldmxcsr fldcw cld lea mov mov call ldmxcsr "
             "fldcw mov mov mov mov mov mov mov mov mov mov mov mov lea mov mov retf"},
            {"resume64", detail::emit_resume64(), true,
             "mov fxrstor mov push popfq mov mov mov mov mov mov mov mov mov mov mov mov lea mov mov retf"},
            {"unwind64", detail::emit_unwind64(0x90000100), true, "mov mov mov mov shr mov jmp"},
            {"id_stub", detail::emit_id_stub(0x201, 0x90010008), false, "mov jmp-far int3 int3 int3 int3 int3"},
            {"patch", detail::emit_patch(0x90010010, 7), false, "call-far nop"},
            {"fs-load", detail::emit_micro_gate(SiteKind::FsLoad, 0, 0x18, 0), true, "mov mov retf"},
            {"fs-store", detail::emit_micro_gate(SiteKind::FsStore, 1, 0x14, 0), true, "mov mov retf"},
            {"fs-store-esp", detail::emit_micro_gate(SiteKind::FsStoreEsp, 4, 0, 0), true, "mov lea mov retf"},
            {"fs-store-imm", detail::emit_micro_gate(SiteKind::FsStoreImm, 0, 0x14, 0x1234), true, "mov mov retf"},
            {"fs-push", detail::emit_micro_gate(SiteKind::FsPush, 0, 0x18, 0), true, "mov mov mov mov mov mov lea retf"},
            {"fs-pop", detail::emit_micro_gate(SiteKind::FsPop, 0, 0x14, 0), true, "mov mov mov mov mov mov lea retf"},
            {"fs-movzx-byte", detail::emit_micro_gate(SiteKind::FsMovzxByte, 0, 0x24, 0), true, "mov movzx retf"},
            {"fs-cmp", detail::emit_micro_gate(SiteKind::FsCmp, 6, 0, 0), true, "mov cmp retf"},
            {"fs-cmp-rev", detail::emit_micro_gate(SiteKind::FsCmpRev, 5, 0x18, 0), true, "mov cmp retf"},
        };
        if (detail::has_micro_gate(SiteKind::FsLoad, 4) || !detail::emit_micro_gate(SiteKind::FsOther, 0, 0, 0).empty())
            return {false, "esp or fs-other has a micro-gate"};
        for (const Case &c : cases) {
            ZydisDecoder d;
            ZydisDecoderInit(&d, c.long64 ? ZYDIS_MACHINE_MODE_LONG_64 : ZYDIS_MACHINE_MODE_LEGACY_32,
                             c.long64 ? ZYDIS_STACK_WIDTH_64 : ZYDIS_STACK_WIDTH_32);
            std::string got;
            size_t off = 0;
            while (off < c.bytes.size()) {
                ZydisDecodedInstruction in;
                ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
                if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&d, c.bytes.data() + off, c.bytes.size() - off, &in, ops)))
                    return {false, format("%s: undecodable at +%zu", c.name, off)};
                std::string m = ZydisMnemonicGetString(in.mnemonic);
                if (in.meta.branch_type == ZYDIS_BRANCH_TYPE_FAR) m = in.mnemonic == ZYDIS_MNEMONIC_RET ? "retf" : m + "-far";
                got += (got.empty() ? "" : " ") + m;
                off += in.length;
            }
            if (got != c.expect) return {false, format("%s: got '%s' want '%s'", c.name, got.c_str(), c.expect)};
        }
        return {true, format("%zu gates", std::size(cases))};
    }

    Result gate()
    {
        g_handlers[100] = [](GuestThread &, GateFrame &f) {
            uint32_t a = arg(f, 0), b = arg(f, 1);
            f.eax = a + b;
            f.edx = a * b;
            stdcall_return(f, 2);
        };
        uint32_t stub = cpu.gate_for(HostCallId::from_raw(100)).v;
        P p;
        p.raw({0x89, 0xE6});   // mov esi, esp
        p.push_imm8(7).push_imm8(5).mov_ecx_imm(stub).call_ecx();
        p.raw({0x39, 0xE6});   // cmp esi, esp
        p.raw({0x74, 0x06});   // je +6
        p.mov_eax_imm(0xBAD).ret();
        p.ret();
        uint32_t va = code.put(p.b);
        snap();
        uint64_t r = run(va);
        uint64_t want = (uint64_t(35) << 32) | 12;
        CpuCounters c = cpu.counters();
        std::string detail = format("edx:eax=0x%llX gate_calls=+%llu", static_cast<unsigned long long>(r),
                                    static_cast<unsigned long long>(c.gate_calls - before.gate_calls));
        return {r == want && c.gate_calls == before.gate_calls + 1, detail};
    }

    struct FsCase {
        const char *name;
        SiteKind kind;
        std::vector<uint8_t> site;
        bool promotable;
    };

    // Each fs form once through the trap path (which promotes it) and once through its micro-gate, same result.
    Result fs_form(const FsCase &fc, std::string &detail)
    {
        detail::XboxKpcr &k = kpcr();
        uint32_t kva = GuestThread::current().kpcr().v;
        P p;
        uint32_t arg_value = 0x5A5A0000 | (uint32_t(fc.kind) << 8);
        uint32_t expect = 0;
        bool checks_field = false;
        switch (fc.kind) {
        case SiteKind::FsLoad:
            p.b = fc.site;
            if (fc.site[1] == 0x8B) p.raw({0x89, 0xF8});   // mov eax, edi
            expect = kva;
            break;
        case SiteKind::FsStore:
            p.raw({0x89, 0xC8});   // mov eax, ecx (the A3 form stores eax, the 89 form stores ecx)
            p.b.insert(p.b.end(), fc.site.begin(), fc.site.end());
            checks_field = true;
            expect = arg_value;
            break;
        case SiteKind::FsStoreEsp:
            p.b = fc.site;
            p.raw({0x89, 0xE0});   // mov eax, esp
            break;
        case SiteKind::FsStoreImm:
            p.b = fc.site;
            checks_field = true;
            expect = 0x12345678;
            break;
        case SiteKind::FsPush:
            p.b = fc.site;
            p.raw({0x58});   // pop eax
            expect = kva;
            break;
        case SiteKind::FsPop:
            p.raw({0x51});   // push ecx
            p.b.insert(p.b.end(), fc.site.begin(), fc.site.end());
            checks_field = true;
            expect = arg_value;
            break;
        case SiteKind::FsMovzxByte:
            k.Irql = 0x5A;
            p.b = fc.site;
            expect = 0x5A;
            break;
        case SiteKind::FsCmp:
            p.mov_eax_imm(kva);
            p.b.insert(p.b.end(), fc.site.begin(), fc.site.end());
            p.raw({0xB8, 0, 0, 0, 0, 0x0F, 0x94, 0xC0});   // mov eax, 0; sete al
            expect = 1;
            break;
        case SiteKind::FsCmpRev:
            p.mov_eax_imm(kva - 1);
            p.b.insert(p.b.end(), fc.site.begin(), fc.site.end());
            p.raw({0xB8, 0, 0, 0, 0, 0x0F, 0x97, 0xC0});   // mov eax, 0; seta al: [0x18] - (kva-1) = 1, above
            expect = 1;
            break;
        case SiteKind::FsOther:
            p.mov_eax_imm(1);
            p.b.insert(p.b.end(), fc.site.begin(), fc.site.end());   // add eax, fs:[0x18]
            expect = kva + 1;
            break;
        default: return {false, "not an fs kind"};
        }
        p.ret();
        uint32_t va = code.put(p.b);
        if (!va) return {false, "no room for the payload"};
        for (int pass = 0; pass < 2; pass++) {
            k.NtTib.ArbitraryUserPointer = 0;
            k.NtTib.ExceptionList = 0;
            snap();
            uint32_t got = uint32_t(run(va, {arg_value}, CallConv::Fastcall));
            CpuCounters c = cpu.counters();
            uint64_t traps = c.traps_fs - before.traps_fs, learned = c.sites_promoted_learned - before.sites_promoted_learned;
            uint64_t unpromotable = c.fs_unpromotable - before.fs_unpromotable;
            if (checks_field) got = k.NtTib.ArbitraryUserPointer;
            if (fc.kind == SiteKind::FsStoreEsp) expect = k.NtTib.ExceptionList;   // the store wrote esp; eax carries esp
            if (fc.kind == SiteKind::FsStoreEsp && expect == 0) return {false, format("pass %d: fs:[0] stayed 0", pass)};
            bool first = pass == 0;
            uint64_t want_traps = fc.promotable ? (first ? 1 : 0) : 1;
            uint64_t want_learned = fc.promotable && first ? 1 : 0;
            uint64_t want_unpromotable = fc.promotable ? 0 : 1;
            detail = format("pass %d: got=0x%08X want=0x%08X traps=%llu learned=%llu unpromotable=%llu", pass, got, expect,
                            static_cast<unsigned long long>(traps), static_cast<unsigned long long>(learned),
                            static_cast<unsigned long long>(unpromotable));
            if (got != expect || traps != want_traps || learned != want_learned || unpromotable != want_unpromotable) return {false, detail};
        }
        if (fc.promotable) {
            std::vector<PatchSite> learned = cpu.learned_sites();
            bool found = false;
            for (const PatchSite &s : learned)
                if (s.kind == fc.kind && s.at.v >= va && s.at.v < va + p.b.size()) found = true;
            if (!found) return {false, detail + " (not in learned_sites)"};
            const uint8_t *site = memory.host<uint8_t>(learned.back().at);
            if (site[0] != 0xFF || site[1] != 0x1D) return {false, detail + format(" (site bytes %02X %02X)", site[0], site[1])};
        }
        return {true, "trap and gate agree, " + detail.substr(detail.find("got="))};
    }

    Result mmio()
    {
        struct Device final : MmioDevice {
            std::string log;
            uint32_t read(GuestVa a, unsigned size) override
            {
                log += format("r%u@%X ", size, a.v & 0xFFF);
                return 0xCAFEBABE;
            }
            void write(GuestVa a, unsigned size, uint32_t v) override { log += format("w%u@%X=%X ", size, a.v & 0xFFF, v); }
        };
        static Device device;
        cpu.map_mmio({{0xFD000000}, 0x1000}, device);
        P p;
        p.mov_edx_imm(0xFD000010);
        p.raw({0x8B, 0x02});                     // mov eax, [edx]
        p.raw({0x89, 0x42, 0x04});               // mov [edx+4], eax
        p.raw({0x66, 0x89, 0x42, 0x08});         // mov [edx+8], ax
        p.raw({0xC6, 0x42, 0x0C, 0x5A});         // mov byte [edx+0xC], 0x5A
        p.raw({0x81, 0x7A, 0x10, 0xBE, 0xBA, 0xFE, 0xCA});   // cmp dword [edx+0x10], 0xCAFEBABE
        p.raw({0x0F, 0x94, 0xC0});               // sete al (eax keeps 0xCAFEBA00 | 1 on equal)
        p.ret();
        uint32_t va = code.put(p.b);
        snap();
        uint32_t r = uint32_t(run(va));
        CpuCounters c = cpu.counters();
        std::string want = "r4@10 w4@14=CAFEBABE w2@18=BABE w1@1C=5A r4@20 ";
        std::string detail = format("eax=0x%08X traps=%llu log='%s'", r, static_cast<unsigned long long>(c.traps_mmio - before.traps_mmio),
                                    device.log.c_str());
        return {r == 0xCAFEBA01 && device.log == want && c.traps_mmio - before.traps_mmio == 5, detail};
    }

    Result port()
    {
        struct Device final : PortDevice {
            std::string log;
            uint32_t in(uint16_t port, unsigned size) override
            {
                log += format("i%u@%X ", size, port);
                return port == 0x60 ? 0x77 : 0x12345678;
            }
            void out(uint16_t port, unsigned size, uint32_t v) override { log += format("o%u@%X=%X ", size, port, v); }
        };
        static Device device;
        cpu.map_ports({0x60, 1}, device);
        cpu.map_ports({0x80C4, 4}, device);
        P p;
        p.raw({0x66, 0xBA, 0xC4, 0x80});   // mov dx, 0x80C4
        p.raw({0xED});                     // in eax, dx
        p.raw({0x89, 0xC3});               // mov ebx, eax
        p.raw({0xEF});                     // out dx, eax
        p.raw({0xE4, 0x60});               // in al, 0x60
        p.raw({0xE6, 0x60});               // out 0x60, al
        p.raw({0x89, 0xD8});               // mov eax, ebx
        p.ret();
        uint32_t va = code.put(p.b);
        snap();
        uint32_t r = uint32_t(run(va));
        CpuCounters c = cpu.counters();
        std::string want = "i4@80C4 o4@80C4=12345678 i1@60 o1@60=77 ";
        std::string detail = format("eax=0x%08X traps=%llu log='%s'", r, static_cast<unsigned long long>(c.traps_port - before.traps_port),
                                    device.log.c_str());
        return {r == 0x12345678 && device.log == want && c.traps_port - before.traps_port == 4, detail};
    }

    Result privileged()
    {
        P p;
        p.raw({0xFA, 0xFB, 0x0F, 0x09, 0x0F, 0x08});   // cli, sti, wbinvd, invd
        p.mov_eax_imm(0x600D).ret();
        uint32_t va = code.put(p.b);
        snap();
        uint32_t r1 = uint32_t(run(va));
        uint64_t traps1 = cpu.counters().traps_privileged - before.traps_privileged;
        snap();
        uint32_t r2 = uint32_t(run(va));
        uint64_t traps2 = cpu.counters().traps_privileged - before.traps_privileged;
        const uint8_t *b = memory.host<uint8_t>({va});
        bool nops = true;
        for (int i = 0; i < 6; i++) nops = nops && b[i] == 0x90;
        std::string detail = format("first=%llu traps then %llu, bytes %02X%02X%02X%02X%02X%02X, eax=0x%X/0x%X",
                                    static_cast<unsigned long long>(traps1), static_cast<unsigned long long>(traps2), b[0], b[1], b[2], b[3], b[4], b[5], r1, r2);
        return {r1 == 0x600D && r2 == 0x600D && traps1 == 4 && traps2 == 0 && nops, detail};
    }

    Result rdtsc()
    {
        constexpr uint32_t kLoops = 20000;
        P p;
        p.mov_ecx_imm(kLoops);
        uint32_t loop = uint32_t(p.b.size());
        p.raw({0x0F, 0x31});   // rdtsc
        p.raw({0x49});         // dec ecx
        p.raw({0x75, uint8_t(int8_t(int(loop) - int(p.b.size()) - 2))});
        p.ret();
        uint32_t va = code.put(p.b);
        PatchSite site{};
        site.at = {va + loop};
        site.length = 2;
        site.kind = SiteKind::Rdtsc;
        site.original[0] = 0x0F;
        site.original[1] = 0x31;
        std::vector<PatchSite> skipped = cpu.apply_sites(std::span<const PatchSite>(&site, 1));
        if (!skipped.empty()) return {false, "apply_sites skipped the rdtsc site"};
        const uint8_t *b = memory.host<uint8_t>({va + loop});
        if (b[0] != 0xEF || b[1] != 0x90) return {false, format("site bytes %02X %02X", b[0], b[1])};
        LARGE_INTEGER f, q0, q1;
        QueryPerformanceFrequency(&f);
        snap();
        QueryPerformanceCounter(&q0);
        uint64_t t0 = cpu.guest_tsc();
        uint64_t t1 = run(va);
        QueryPerformanceCounter(&q1);
        double seconds = double(q1.QuadPart - q0.QuadPart) / double(f.QuadPart);
        double hz = double(t1 - t0) / seconds;
        double want = 733'333'333.0;
        double error = (hz - want) / want;
        uint64_t traps = cpu.counters().traps_rdtsc - before.traps_rdtsc;
        std::string detail = format("%.0f Hz over %.1f ms, error %.3f%%, traps=%llu, %.0f ns per trap", hz, seconds * 1e3,
                                    error * 100, static_cast<unsigned long long>(traps), seconds * 1e9 / kLoops);
        return {traps == kLoops && error > -0.01 && error < 0.01, detail};
    }

    Result nested()
    {
        P c;
        c.raw({0x8B, 0x44, 0x24, 0x04});   // mov eax, [esp+4]
        c.raw({0x05}).u32(1000);           // add eax, 1000
        c.raw({0xC2, 0x04, 0x00});         // ret 4
        uint32_t payload_c = code.put(c.b);
        uint32_t stub102 = cpu.gate_for(HostCallId::from_raw(102)).v;
        P b;
        b.raw({0x8B, 0x44, 0x24, 0x04});   // mov eax, [esp+4]
        b.raw({0x40});                     // inc eax
        b.raw({0x50});                     // push eax
        b.mov_ecx_imm(stub102).call_ecx();
        b.raw({0xC2, 0x04, 0x00});         // ret 4
        uint32_t payload_b = code.put(b.b);
        uint32_t stub101 = cpu.gate_for(HostCallId::from_raw(101)).v;
        P a;
        a.push_imm8(10).mov_ecx_imm(stub101).call_ecx().ret();
        uint32_t payload_a = code.put(a.b);
        static uint32_t max_depth;
        max_depth = 0;
        g_handlers[101] = [payload_b](GuestThread &t, GateFrame &f) {
            uint32_t v = arg(f, 0);
            uint32_t r = uint32_t(t.call({payload_b}, std::vector<uint32_t>{v + 1}, CallConv::Stdcall));
            f.eax = r + 100;
            stdcall_return(f, 1);
        };
        g_handlers[102] = [payload_c](GuestThread &t, GateFrame &f) {
            uint32_t v = arg(f, 0);
            max_depth = detail::block_of(t)->host.depth;
            uint32_t r = uint32_t(t.call({payload_c}, std::vector<uint32_t>{v}, CallConv::Stdcall));
            f.eax = r * 2;
            stdcall_return(f, 1);
        };
        snap();
        uint32_t r = uint32_t(run(payload_a));
        uint64_t calls = cpu.counters().guest_calls - before.guest_calls, gates = cpu.counters().gate_calls - before.gate_calls;
        uint32_t depth_after = detail::block_of(GuestThread::current())->host.depth;
        std::string detail = format("eax=%u guest_calls=%llu gate_calls=%llu depth in the inner handler=%u, after=%u", r,
                                    static_cast<unsigned long long>(calls), static_cast<unsigned long long>(gates), max_depth, depth_after);
        return {r == 2124 && calls == 3 && gates == 2 && max_depth == 2 && depth_after == 0, detail};
    }

    Result spawn_exit()
    {
        uint32_t stub104 = cpu.gate_for(HostCallId::from_raw(104)).v;
        P f;
        f.mov_ecx_imm(stub104).call_ecx().ret();
        uint32_t payload_f = code.put(f.b);
        uint32_t stub103 = cpu.gate_for(HostCallId::from_raw(103)).v;
        P e;
        e.push_imm8(5).mov_ecx_imm(stub103).call_ecx().mov_eax_imm(0xBAD).ret();
        uint32_t payload_e = code.put(e.b);
        static uint32_t exit_depth, spawned_index;
        static bool tls;
        g_handlers[103] = [payload_f](GuestThread &t, GateFrame &f) {
            t.call({payload_f}, std::vector<uint32_t>{}, CallConv::Stdcall);
            f.eax = 0xBAD;
            stdcall_return(f, 1);
        };
        g_handlers[104] = [&memory = memory](GuestThread &t, GateFrame &) {
            const ThreadBlock *tb = detail::block_of(t);
            uint32_t data = tb->ethread.Tcb.TlsData;
            tls = data != 0 && *memory.host<uint32_t>({data}) == data + 4 && tb->kpcr.NtTib.StackBase == data + 0x40 &&
                  tb->host.stack_top <= data;
            exit_depth = tb->host.depth;
            spawned_index = t.index();
            t.exit(0x77);
        };
        snap();
        g_fatals = 0;
        g_fatal = {};
        SpawnOptions options{0x20000, 8, false, 0x40};
        GuestThread &t = GuestThread::spawn({payload_e}, std::span<const uint32_t>{}, options);
        HANDLE h = detail::thread_handle(t);
        DWORD wait = WaitForSingleObject(h, 10000);
        DWORD code = 0;
        GetExitCodeThread(h, &code);
        uint32_t threads_now = cpu.counters().live_guest_threads;
        const ThreadBlock *tb = detail::block_of(t);
        uint32_t monitor = tb->kpcr.PrcbData.DebugMonitorData;
        uint32_t notify = monitor ? *memory.host<uint32_t>({monitor + detail::kDebugMonitorNotify}) : 0;
        std::string detail = format("wait=%lu exit=0x%lX depth at exit=%u index=%u threads=%u tls=%d prcb+250=0x%08X notify=0x%08X fatal='%s' at 0x%08X",
                                    wait, code, exit_depth, spawned_index, threads_now, int(tls), monitor, notify,
                                    g_fatals ? g_fatal.what.c_str() : "", g_fatals ? g_fatal.where.v : 0);
        if (g_fatals)
            detail += format(" esp=0x%08X ebp=0x%08X eflags=0x%08X eax=0x%08X ecx=0x%08X", g_fatal.esp, g_fatal.ebp, g_fatal.eflags, g_fatal.eax,
                             g_fatal.ecx);
        return {wait == WAIT_OBJECT_0 && code == 0x77 && exit_depth == 2 && spawned_index == t.index() && spawned_index > 0 && threads_now == 0 && tls &&
                    notify == detail::g_cpu->gates.dm_notify,
                detail};
    }

    // Defect: a ThreadBlock that outlives its thread. A title that starts and ends threads for ever then runs the
    // runtime region out of blocks; one hundred threads in turn must share a handful.
    Result thread_reuse()
    {
        uint32_t stub = cpu.gate_for(HostCallId::from_raw(110)).v;
        P p;
        p.mov_ecx_imm(stub).call_ecx().ret();
        uint32_t payload = code.put(p.b);
        static std::set<uint32_t> blocks;
        blocks.clear();
        g_handlers[110] = [](GuestThread &t, GateFrame &f) {
            blocks.insert(t.kpcr().v);
            stdcall_return(f, 0);
        };
        snap();
        DWORD waits = 0;
        for (int i = 0; i < 100; i++) {
            GuestThread &t = GuestThread::spawn({payload}, std::span<const uint32_t>{}, SpawnOptions{0x10000, 8, false, 0});
            if (WaitForSingleObject(detail::thread_handle(t), 10000) != WAIT_OBJECT_0) waits++;
        }
        uint32_t threads_now = cpu.counters().live_guest_threads;
        std::string detail = format("%zu distinct ThreadBlocks for 100 threads run one after another, %lu joins timed out, threads=%u",
                                    blocks.size(), waits, threads_now);
        return {blocks.size() <= 2 && waits == 0 && threads_now == 0, detail};
    }

    // Defect: a thread left suspended inside an MMIO access, holding the device's lock, or one never suspended once
    // it is outside.
    Result suspend_scope()
    {
        struct Device final : MmioDevice {
            HANDLE parked = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            std::atomic<bool> park{true}, stop{false};
            std::atomic<uint32_t> reads{0};
            uint32_t read(GuestVa, unsigned) override
            {
                reads++;
                if (park) {
                    SetEvent(parked);
                    WaitForSingleObject(release, 10000);
                }
                return stop ? 1 : 0;
            }
            void write(GuestVa, unsigned, uint32_t) override {}
        };
        static Device device;
        cpu.map_mmio({{0xFD003000}, 0x1000}, device);
        P p;
        p.mov_edx_imm(0xFD003000);
        p.raw({0x8B, 0x02, 0x85, 0xC0, 0x74, 0xFA});   // again: mov eax, [edx]; test eax, eax; jz again
        p.ret();
        uint32_t payload = code.put(p.b);
        GuestThread &t = GuestThread::spawn({payload}, std::span<const uint32_t>{}, SpawnOptions{0x20000, 8, false, 0});
        HANDLE h = detail::thread_handle(t);
        DWORD parked = WaitForSingleObject(device.parked, 5000);
        uint32_t previous = 0;
        auto start = std::chrono::steady_clock::now();
        SuspendResult inside = suspend_outside_devices(h, &previous);
        auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        device.park = false;
        SetEvent(device.release);
        SuspendResult outside = suspend_outside_devices(h, &previous);
        uint32_t before = device.reads;
        Sleep(30);
        uint32_t after = device.reads;
        ResumeThread(h);
        device.stop = true;
        DWORD exited = WaitForSingleObject(h, 5000);
        std::string detail = format("parked=%lu inside=%d after %lld ms, outside=%d previous=%u reads while suspended=%u exited=%lu",
                                    parked, int(inside), static_cast<long long>(waited), int(outside), previous, after - before, exited);
        return {parked == WAIT_OBJECT_0 && inside == SuspendResult::InsideDevice && waited >= 10 && outside == SuspendResult::Suspended &&
                    previous == 0 && after == before && exited == WAIT_OBJECT_0,
                detail};
    }

    Result fault_gate()
    {
        uint32_t ptr = code.put(detail::emit_far_pointer(detail::g_cpu->gates.fault_gate, detail::kLongCs));
        P g;
        g.mov_eax_imm(0x1111).jmp_far_ptr(ptr).ret();
        uint32_t payload = code.put(g.b);
        g_fatals = 0;
        g_fatal = {};
        SpawnOptions options{0x20000, 8, false, 0};
        GuestThread &t = GuestThread::spawn({payload}, std::span<const uint32_t>{}, options);
        HANDLE h = detail::thread_handle(t);
        DWORD wait = WaitForSingleObject(h, 10000);
        DWORD code_out = 0;
        GetExitCodeThread(h, &code_out);
        std::string detail = format("wait=%lu exit=0x%lX fatals=%d thread=%u eax=0x%X what='%s'", wait, code_out, g_fatals, g_fatal.thread, g_fatal.eax,
                                    g_fatal.what.c_str());
        bool named = g_fatal.what.find("inside the gates") != std::string::npos;
        return {wait == WAIT_OBJECT_0 && code_out == detail::kFatalExitCode && g_fatals == 1 && g_fatal.thread == t.index() && named && g_fatal.eax == 0x1111,
                detail};
    }

    Result trap_state()
    {
        P p;
        p.mov_eax_imm(0x3F800000);
        p.raw({0x66, 0x0F, 0x6E, 0xF8});   // movd xmm7, eax
        p.raw({0xBB}).u32(0x11223344);     // mov ebx, imm
        p.raw({0xF9});                     // stc
        p.raw({0xF4});                     // hlt
        p.raw({0xB8, 0, 0, 0, 0});         // mov eax, 0
        p.raw({0x83, 0xD0, 0x00});         // adc eax, 0: CF survived?
        p.raw({0x66, 0x0F, 0x7E, 0xFA});   // movd edx, xmm7
        p.raw({0x81, 0xFB}).u32(0x11223344);   // cmp ebx, imm
        p.raw({0x75, 0x05});               // jne +5
        p.raw({0x0D}).u32(0x100);          // or eax, 0x100
        p.ret();
        uint32_t va = code.put(p.b);
        snap();
        uint64_t r = run(va);
        uint64_t traps = cpu.counters().traps_privileged - before.traps_privileged;
        std::string detail = format("edx:eax=0x%llX traps=%llu", static_cast<unsigned long long>(r), static_cast<unsigned long long>(traps));
        return {r == ((uint64_t(0x3F800000) << 32) | 0x101) && traps == 1, detail};
    }

    Result memory_verbs()
    {
        using namespace xbox_mem;
        using namespace xbox_page;
        std::vector<std::string> fails;
        auto check = [&fails](bool ok, const std::string &what) {
            if (!ok) fails.push_back(what);
        };
        GuestMemory::Statistics s0 = memory.statistics();
        check(s0.total_pages == memory.ram_bytes() / 0x1000, "total pages");

        GuestVa base{};
        uint32_t size = 0x3000;
        check(memory.allocate_virtual(&base, &size, Reserve | Commit, ReadWrite) == xbox_status::Success, "reserve+commit");
        check(base.v != 0 && (base.v & 0xFFFF) == 0 && size == 0x3000, format("placement 0x%08X size 0x%X", base.v, size));
        if (base.v) {
            *memory.host<uint32_t>(base) = 0x12345678;
            check(*memory.host<uint32_t>({0x80000000 + base.v}) == 0x12345678, "committed page aliases its physical page");
            XboxMemoryBasicInformation q{};
            check(memory.query_virtual(base + 0x1000, &q) == xbox_status::Success && q.state == Commit && q.protect == ReadWrite &&
                      q.allocation_base == base.v && q.region_size == 0x2000 && q.base_address == base.v + 0x1000,
                  format("query committed: state 0x%X protect 0x%X alloc 0x%08X size 0x%X", q.state, q.protect, q.allocation_base, q.region_size));
            check(memory.statistics().user_pages == s0.user_pages + 3, "user pages +3");

            GuestVa pb = base;
            uint32_t ps = 0x1000, old = 0;
            check(memory.protect_virtual(&pb, &ps, ReadOnly, &old) == xbox_status::Success && old == ReadWrite, format("protect old 0x%X", old));
            check(memory.query_virtual(base, &q) == xbox_status::Success && q.protect == ReadOnly && q.region_size == 0x1000, "query after protect");
            check(memory.query_virtual(base + 0x1000, &q) == xbox_status::Success && q.protect == ReadWrite && q.region_size == 0x2000, "query the rest");
            ps = 0x1000;
            check(memory.protect_virtual(&pb, &ps, ReadWrite, &old) == xbox_status::Success, "protect back");

            GuestVa fb = base + 0x2000;
            uint32_t fs = 0x1000;
            check(memory.free_virtual(&fb, &fs, Decommit) == xbox_status::Success, "decommit one page");
            check(memory.query_virtual(base + 0x2000, &q) == xbox_status::Success && q.state == Reserve && q.region_size == 0x1000, "query decommitted");
            check(memory.statistics().user_pages == s0.user_pages + 2, "user pages +2");
            check(memory.allocation_size(base + 0x100) == 0x3000, "allocation size of the reservation");

            fb = base;
            fs = 0;
            check(memory.free_virtual(&fb, &fs, Release) == xbox_status::Success && fs == 0x3000, "release whole");
            check(memory.query_virtual(base, &q) == xbox_status::Success && q.state == Free, "query released");
            check(memory.statistics().user_pages == s0.user_pages, "user pages back");
        }

        GuestVa top{};
        uint32_t top_size = 0x1000;
        check(memory.allocate_virtual(&top, &top_size, Reserve | Commit | TopDown, ReadWrite) == xbox_status::Success && top.v >= memory.ram_bytes() - 0x1000000,
              format("top down placed at 0x%08X", top.v));
        if (top.v) {
            uint32_t z = 0;
            memory.free_virtual(&top, &z, Release);
        }

        GuestVa fixed{0x00400000};
        uint32_t fixed_size = 0x20000;
        check(memory.allocate_virtual(&fixed, &fixed_size, Reserve, ReadWrite) == xbox_status::Success && fixed.v == 0x00400000, "fixed reserve");
        GuestVa again{0x00400000};
        uint32_t again_size = 0x1000;
        check(memory.allocate_virtual(&again, &again_size, Reserve, ReadWrite) == xbox_status::ConflictingAddresses, "conflicting reserve");
        GuestVa inside{0x00401000};
        uint32_t inside_size = 0x1000;
        check(memory.allocate_virtual(&inside, &inside_size, Commit, ExecuteReadWrite) == xbox_status::Success && inside.v == 0x00401000, "commit inside");
        XboxMemoryBasicInformation q{};
        check(memory.query_virtual({0x00401000}, &q) == xbox_status::Success && q.state == Commit && q.protect == ExecuteReadWrite && q.allocation_base == 0x00400000,
              "query inside the fixed reservation");
        GuestVa beyond{0x00410000};
        uint32_t beyond_size = 0x20000;
        check(memory.allocate_virtual(&beyond, &beyond_size, Commit, ReadWrite) == xbox_status::ConflictingAddresses, "commit past the reservation");
        uint32_t zero = 0;
        check(memory.free_virtual(&fixed, &zero, Release) == xbox_status::Success, "release fixed");
        GuestVa unaligned{0x00501000};
        uint32_t unaligned_size = 0x1000;
        check(memory.allocate_virtual(&unaligned, &unaligned_size, Reserve | Commit, ReadWrite) == xbox_status::Success && unaligned.v == 0x00500000 &&
                  unaligned_size == 0x2000,
              format("reserve+commit at an unaligned base: 0x%08X+0x%X", unaligned.v, unaligned_size));
        if (unaligned.v) memory.free_virtual(&unaligned, &zero, Release);

        GuestVa bad{};
        uint32_t bad_size = 0;
        check(memory.allocate_virtual(&bad, &bad_size, Reserve, ReadWrite) == xbox_status::InvalidParameter, "size 0");
        bad_size = 0x1000;
        check(memory.allocate_virtual(&bad, &bad_size, Reserve, 3) == xbox_status::InvalidPageProtection, "protect 3");
        check(memory.allocate_virtual(&bad, &bad_size, 0x40, ReadWrite) == xbox_status::InvalidParameter, "type 0x40");
        GuestVa nowhere{0x00600000};
        uint32_t nowhere_size = 0x1000;
        check(memory.free_virtual(&nowhere, &nowhere_size, Release) == xbox_status::MemoryNotAllocated, "free unallocated");

        GuestVa c = memory.allocate_contiguous(0x5000, 0, 0x03FFFFFF, 0x10000, ReadWrite);
        check(c.v >= 0x80000000 && ((c.v - 0x80000000) & 0xFFFF) == 0, format("contiguous at 0x%08X", c.v));
        if (c.v) {
            uint32_t pa = c.v - 0x80000000;
            check(memory.physical(c) == pa && memory.physical({pa}) == pa && memory.physical({0xF0000000 + pa}) == pa, "physical of the three views");
            *memory.host<uint32_t>(c) = 0xC0FFEE;
            check(*memory.host<uint32_t>({0xF0000000 + pa}) == 0xC0FFEE, "contiguous aliases tiled");
            check(memory.allocation_size(c) == 0x5000, "contiguous allocation size");
            check(memory.statistics().contiguous_pages == s0.contiguous_pages + 5, "contiguous pages +5");
            GuestVa user_over{pa};
            uint32_t uo = 0x1000;
            check(memory.allocate_virtual(&user_over, &uo, Reserve, ReadWrite) == xbox_status::ConflictingAddresses, "user reserve over contiguous pages");
            memory.free_contiguous(c);
            check(memory.statistics().contiguous_pages == s0.contiguous_pages, "contiguous pages back");
        }
        GuestVa sys = memory.allocate_system(0x2000, ReadWrite);
        check(sys.v >= 0xD0000000 && sys.v < 0xF0000000, format("system at 0x%08X", sys.v));
        if (sys.v) {
            *memory.host<uint32_t>(sys) = 1;
            check(memory.statistics().system_pages == s0.system_pages + 2, "system pages +2");
            check(memory.allocation_size(sys) == 0x2000, "system allocation size");
            memory.free_system(sys, 0x2000);
            check(memory.statistics().system_pages == s0.system_pages, "system pages back");
        }
        GuestMemory::Statistics s1 = memory.statistics();
        check(s1.available_pages == s1.total_pages - s1.user_pages - s1.contiguous_pages - s1.system_pages - s1.stack_pages, "available adds up");
        std::string detail = fails.empty() ? format("%u checks, available %u of %u pages", 41u, s1.available_pages, s1.total_pages) : "";
        for (const std::string &f : fails) detail += (detail.empty() ? "" : "; ") + f;
        return {fails.empty(), detail};
    }
};

}  // namespace

bool run_cpu_tests(GuestMemory &memory, const std::function<void(const std::string &)> &sink)
{
    {
        int regs[4];
        char brand[49] = {};
        for (int i = 0; i < 3; i++) {
            __cpuid(regs, int(0x80000002 + i));
            memcpy(brand + 16 * i, regs, 16);
        }
        sink(std::string("cpu host ") + brand);
    }
    constexpr uint32_t kCodeSpan = 0x10000;
    GuestVa span = memory.runtime_alloc(kCodeSpan, 0x1000, true);
    if (!span.v) {
        sink("cpu emit FAIL no RWX span for the payloads");
        return false;
    }
    CpuOptions options;
    options.pin_guest_threads_to_one_core = true;
    Cpu &cpu = Cpu::install(memory, on_host_call, on_fatal, options);
    Tests t{memory, cpu, {&memory, span.v, span.v + kCodeSpan}};
    bool all = true;
    auto report = [&](const char *name, Result r) {
        sink(std::string("cpu ") + name + (r.ok ? " ok " : " FAIL ") + r.detail);
        all = all && r.ok;
    };
    report("emit", t.emit());
    report("gate", t.gate());
    const Tests::FsCase forms[] = {
        {"fs-load", SiteKind::FsLoad, {0x64, 0xA1, 0x18, 0, 0, 0}, true},
        {"fs-load-8b", SiteKind::FsLoad, {0x64, 0x8B, 0x3D, 0x18, 0, 0, 0}, true},
        {"fs-store", SiteKind::FsStore, {0x64, 0x89, 0x0D, 0x14, 0, 0, 0}, true},
        {"fs-store-a3", SiteKind::FsStore, {0x64, 0xA3, 0x14, 0, 0, 0}, true},
        {"fs-store-esp", SiteKind::FsStoreEsp, {0x64, 0x89, 0x25, 0, 0, 0, 0}, true},
        {"fs-store-imm", SiteKind::FsStoreImm, {0x64, 0xC7, 0x05, 0x14, 0, 0, 0, 0x78, 0x56, 0x34, 0x12}, true},
        {"fs-push", SiteKind::FsPush, {0x64, 0xFF, 0x35, 0x18, 0, 0, 0}, true},
        {"fs-pop", SiteKind::FsPop, {0x64, 0x8F, 0x05, 0x14, 0, 0, 0}, true},
        {"fs-movzx-byte", SiteKind::FsMovzxByte, {0x64, 0x0F, 0xB6, 0x05, 0x24, 0, 0, 0}, true},
        {"fs-cmp", SiteKind::FsCmp, {0x64, 0x3B, 0x05, 0x18, 0, 0, 0}, true},
        {"fs-cmp-rev", SiteKind::FsCmpRev, {0x64, 0x39, 0x05, 0x18, 0, 0, 0}, true},
        {"fs-other", SiteKind::FsOther, {0x64, 0x03, 0x05, 0x18, 0, 0, 0}, false},
    };
    for (const Tests::FsCase &fc : forms) {
        std::string detail;
        Result r = t.fs_form(fc, detail);
        report(fc.name, r);
    }
    report("mmio", t.mmio());
    report("port", t.port());
    report("privileged", t.privileged());
    report("rdtsc", t.rdtsc());
    report("nested", t.nested());
    report("trap-state", t.trap_state());
    report("spawn-exit", t.spawn_exit());
    report("thread-reuse", t.thread_reuse());
    report("suspend-scope", t.suspend_scope());
    report("fault-gate", t.fault_gate());
    report("memory", t.memory_verbs());
    CpuCounters c = cpu.counters();
    sink(format("cpu counters gate_calls=%llu guest_calls=%llu traps fs=%llu mmio=%llu port=%llu rdtsc=%llu privileged=%llu patch_wait=%llu "
                "promoted eager=%llu learned=%llu fs_unpromotable=%llu threads=%u",
                static_cast<unsigned long long>(c.gate_calls), static_cast<unsigned long long>(c.guest_calls), static_cast<unsigned long long>(c.traps_fs),
                static_cast<unsigned long long>(c.traps_mmio), static_cast<unsigned long long>(c.traps_port), static_cast<unsigned long long>(c.traps_rdtsc),
                static_cast<unsigned long long>(c.traps_privileged), static_cast<unsigned long long>(c.traps_patch_wait),
                static_cast<unsigned long long>(c.sites_promoted_eager), static_cast<unsigned long long>(c.sites_promoted_learned),
                static_cast<unsigned long long>(c.fs_unpromotable), c.live_guest_threads));
    return all;
}

}  // namespace xbrt
