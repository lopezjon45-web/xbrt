#include "host/cpu/cpu_internal.h"

#include <initializer_list>

namespace xbrt::detail {

namespace {

enum Reg : unsigned { rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8, r9, r10, r11, r12, r13, r14, r15 };
constexpr int32_t kShadow = 0x20;
constexpr int32_t kGateFrameSize = 0x40;

struct X64 {
    std::vector<uint8_t> b;

    void byte(uint8_t v) { b.push_back(v); }
    void bytes(std::initializer_list<uint8_t> l) { b.insert(b.end(), l); }
    void u32(uint32_t v)
    {
        for (int i = 0; i < 4; i++) byte(uint8_t(v >> (8 * i)));
    }
    void u64(uint64_t v)
    {
        for (int i = 0; i < 8; i++) byte(uint8_t(v >> (8 * i)));
    }
    void rex(bool w, unsigned reg, unsigned base)
    {
        uint8_t r = uint8_t(0x40 | (w ? 8 : 0) | ((reg >> 3) & 1) << 2 | ((base >> 3) & 1));
        if (r != 0x40) byte(r);
    }
    static uint8_t modrm(unsigned mod, unsigned reg, unsigned rm) { return uint8_t(mod << 6 | (reg & 7) << 3 | (rm & 7)); }
    // [base + disp]: rsp and r12 need a SIB byte, rbp and r13 need an explicit displacement.
    void mem(unsigned reg, unsigned base, int32_t disp)
    {
        unsigned rm = base & 7;
        bool sib = rm == 4;
        if (disp == 0 && rm != 5) {
            byte(modrm(0, reg, rm));
        } else if (disp >= -128 && disp <= 127) {
            byte(modrm(1, reg, rm));
            if (sib) byte(0x24);
            byte(uint8_t(disp));
            return;
        } else {
            byte(modrm(2, reg, rm));
            if (sib) byte(0x24);
            u32(uint32_t(disp));
            return;
        }
        if (sib) byte(0x24);
    }
    void op_mem(bool w, std::initializer_list<uint8_t> opcode, unsigned reg, unsigned base, int32_t disp, uint8_t prefix = 0)
    {
        if (prefix) byte(prefix);
        rex(w, reg, base);
        bytes(opcode);
        mem(reg, base, disp);
    }
    void op_rr(bool w, std::initializer_list<uint8_t> opcode, unsigned reg, unsigned rm)
    {
        rex(w, reg, rm);
        bytes(opcode);
        byte(modrm(3, reg, rm));
    }

    void push(unsigned r) { rex(false, 0, r); byte(uint8_t(0x50 | (r & 7))); }
    void pop(unsigned r) { rex(false, 0, r); byte(uint8_t(0x58 | (r & 7))); }
    void sub_rsp(uint8_t imm) { bytes({0x48, 0x83, 0xEC, imm}); }
    void add_rsp(uint8_t imm) { bytes({0x48, 0x83, 0xC4, imm}); }
    void movdqa_store(unsigned base, int32_t disp, unsigned xmm) { op_mem(false, {0x0F, 0x7F}, xmm, base, disp, 0x66); }
    void movdqa_load(unsigned xmm, unsigned base, int32_t disp) { op_mem(false, {0x0F, 0x6F}, xmm, base, disp, 0x66); }
    void mov_rr64(unsigned dst, unsigned src) { op_rr(true, {0x89}, src, dst); }
    void mov_rr32(unsigned dst, unsigned src) { op_rr(false, {0x89}, src, dst); }
    void load64(unsigned reg, unsigned base, int32_t disp) { op_mem(true, {0x8B}, reg, base, disp); }
    void store64(unsigned base, int32_t disp, unsigned reg) { op_mem(true, {0x89}, reg, base, disp); }
    void load32(unsigned reg, unsigned base, int32_t disp) { op_mem(false, {0x8B}, reg, base, disp); }
    void store32(unsigned base, int32_t disp, unsigned reg) { op_mem(false, {0x89}, reg, base, disp); }
    void store_imm32(unsigned base, int32_t disp, uint32_t imm) { op_mem(false, {0xC7}, 0, base, disp); u32(imm); }
    void movzx8(unsigned reg, unsigned base, int32_t disp) { op_mem(false, {0x0F, 0xB6}, reg, base, disp); }
    void cmp_rm(unsigned reg, unsigned base, int32_t disp) { op_mem(false, {0x3B}, reg, base, disp); }
    void cmp_mr(unsigned base, int32_t disp, unsigned reg) { op_mem(false, {0x39}, reg, base, disp); }
    void ldmxcsr(unsigned base, int32_t disp) { op_mem(false, {0x0F, 0xAE}, 2, base, disp); }
    void stmxcsr(unsigned base, int32_t disp) { op_mem(false, {0x0F, 0xAE}, 3, base, disp); }
    void fxrstor(unsigned base, int32_t disp) { op_mem(false, {0x0F, 0xAE}, 1, base, disp); }
    void fldcw(unsigned base, int32_t disp) { op_mem(false, {0xD9}, 5, base, disp); }
    void fnstcw(unsigned base, int32_t disp) { op_mem(false, {0xD9}, 7, base, disp); }
    void mov_ax(uint16_t imm) { bytes({0x66, 0xB8, uint8_t(imm), uint8_t(imm >> 8)}); }
    void mov_fs_ax() { bytes({0x8E, 0xE0}); }
    void xor32(unsigned r) { op_rr(false, {0x31}, r, r); }
    void lea64(unsigned reg, unsigned base, int32_t disp) { op_mem(true, {0x8D}, reg, base, disp); }
    void lea32(unsigned reg, unsigned base, int32_t disp) { op_mem(false, {0x8D}, reg, base, disp); }
    void mov_imm64(unsigned reg, uint64_t imm) { rex(true, 0, reg); byte(uint8_t(0xB8 | (reg & 7))); u64(imm); }
    void call(unsigned reg) { rex(false, 0, reg); byte(0xFF); byte(modrm(3, 2, reg)); }
    void jmp(unsigned reg) { rex(false, 0, reg); byte(0xFF); byte(modrm(3, 4, reg)); }
    void shl64(unsigned reg, uint8_t n) { rex(true, 0, reg); bytes({0xC1, modrm(3, 4, reg), n}); }
    void shr64(unsigned reg, uint8_t n) { rex(true, 0, reg); bytes({0xC1, modrm(3, 5, reg), n}); }
    void or64(unsigned dst, unsigned src) { op_rr(true, {0x09}, src, dst); }
    void popfq() { byte(0x9D); }
    void retf() { byte(0xCB); }
    // AMD processors can return from a system call with SS's hidden descriptor unusable; 64-bit code never notices,
    // but compat-mode code faults on its first stack access (measured on EPYC CI hosts). A reload refreshes it.
    void reload_ss()
    {
        bytes({0x41, 0xB9});   // mov r9d, kFlatData
        u32(kFlatData);
        bytes({0x41, 0x8E, 0xD1});   // mov ss, r9w
    }
    void ret() { byte(0xC3); }
    void cld() { byte(0xFC); }
    void ud2() { bytes({0x0F, 0x0B}); }

    void far_return_to_guest(unsigned esp_reg, unsigned eip_reg)
    {
        reload_ss();
        mov_rr32(rsp, esp_reg);
        lea64(rsp, rsp, -8);
        store32(rsp, 0, eip_reg);
        store_imm32(rsp, 4, kCompatCs);
        retf();
    }
};

struct FrameReg {
    unsigned reg;
    int32_t off;
};
const FrameReg kFrameRegs[] = {{rax, offsetof(GateFrame, eax)}, {rcx, offsetof(GateFrame, ecx)}, {rdx, offsetof(GateFrame, edx)},
                               {rbx, offsetof(GateFrame, ebx)}, {rbp, offsetof(GateFrame, ebp)}, {rsi, offsetof(GateFrame, esi)},
                               {rdi, offsetof(GateFrame, edi)}};

}  // namespace

std::vector<uint8_t> emit_enter64()
{
    X64 a;
    for (unsigned r : {rbx, rbp, rsi, rdi, r12, r13, r14, r15}) a.push(r);
    a.sub_rsp(0x28);
    a.movdqa_store(rsp, 0x00, 6);
    a.movdqa_store(rsp, 0x10, 7);
    a.mov_rr64(r15, rcx);
    a.load64(rax, r15, kHostRsp);
    a.store64(rsp, 0x20, rax);
    a.store64(r15, kHostRsp, rsp);
    a.ldmxcsr(r15, kGuestMxcsr);
    a.fldcw(r15, kGuestFpcw);
    a.mov_ax(kFlatData);
    a.mov_fs_ax();
    a.load32(r8, rdx, offsetof(EntryState, eip));
    a.load32(rcx, rdx, offsetof(EntryState, ecx));
    a.load32(r9, rdx, offsetof(EntryState, edx));
    a.load32(rdx, rdx, offsetof(EntryState, esp));
    a.mov_rr32(rsp, rdx);
    a.lea64(rsp, rsp, -8);
    a.store32(rsp, 0, r8);
    a.store_imm32(rsp, 4, kCompatCs);
    a.mov_rr32(rdx, r9);
    a.reload_ss();
    for (unsigned r : {rax, rbx, rbp, rsi, rdi}) a.xor32(r);
    a.retf();
    return a.b;
}

std::vector<uint8_t> emit_exit32(uint32_t ptr_exit64)
{
    X64 a;
    a.bytes({0xFF, 0x2D});   // jmp far [ptr], 32-bit absolute
    a.u32(ptr_exit64);
    return a.b;
}

std::vector<uint8_t> emit_exit64()
{
    X64 a;
    a.load64(rsp, r15, kHostRsp);
    a.stmxcsr(r15, kGuestMxcsr);
    a.fnstcw(r15, kGuestFpcw);
    a.ldmxcsr(r15, kHostMxcsr);
    a.fldcw(r15, kHostFpcw);
    a.cld();
    a.mov_rr32(rax, rax);
    a.shl64(rdx, 32);
    a.or64(rax, rdx);
    a.load64(r10, rsp, 0x20);
    a.store64(r15, kHostRsp, r10);
    a.movdqa_load(6, rsp, 0x00);
    a.movdqa_load(7, rsp, 0x10);
    a.add_rsp(0x28);
    for (unsigned r : {r15, r14, r13, r12, rdi, rsi, rbp, rbx}) a.pop(r);
    a.ret();
    return a.b;
}

std::vector<uint8_t> emit_full_gate64(uint64_t on_gate)
{
    X64 a;
    a.mov_rr32(r11, rsp);
    a.load64(rsp, r15, kHostRsp);
    a.lea64(rsp, rsp, -(kShadow + kGateFrameSize));
    for (const FrameReg &r : kFrameRegs) a.store32(rsp, kShadow + r.off, r.reg);
    a.store32(rsp, kShadow + offsetof(GateFrame, esp), r11);
    a.load32(r10, r11, 0);
    a.store32(rsp, kShadow + offsetof(GateFrame, eip), r10);
    a.stmxcsr(rsp, kShadow + offsetof(GateFrame, mxcsr));
    a.fnstcw(rsp, kShadow + offsetof(GateFrame, fpcw));
    a.ldmxcsr(r15, kHostMxcsr);
    a.fldcw(r15, kHostFpcw);
    a.cld();
    a.lea64(rcx, rsp, kShadow);
    a.mov_rr64(rdx, r15);
    a.mov_imm64(rax, on_gate);
    a.call(rax);
    a.ldmxcsr(rsp, kShadow + offsetof(GateFrame, mxcsr));
    a.fldcw(rsp, kShadow + offsetof(GateFrame, fpcw));
    for (const FrameReg &r : kFrameRegs) a.load32(r.reg, rsp, kShadow + r.off);
    a.load32(r10, rsp, kShadow + offsetof(GateFrame, eip));
    a.load32(r11, rsp, kShadow + offsetof(GateFrame, esp));
    a.far_return_to_guest(r11, r10);
    return a.b;
}

// gates.md 5 with the interrogate's cleaner shape: the whole guest CONTEXT comes back from the ThreadBlock's copy
// (GPRs, EFLAGS, the x87/SSE state through fxrstor), and the far-return frame is built here, after NtContinue is
// done with the guest stack, instead of under the kernel's exception frame.
std::vector<uint8_t> emit_resume64()
{
    X64 a;
    a.load64(r15, rcx, ctx::R15);
    a.fxrstor(rcx, ctx::FltSave);
    a.load32(r10, rcx, ctx::EFlags);
    a.push(r10);
    a.popfq();
    a.load32(r11, rcx, ctx::Rsp);
    a.load32(r10, rcx, ctx::Rip);
    a.load32(rax, rcx, ctx::Rax);
    a.load32(rdx, rcx, ctx::Rdx);
    a.load32(rbx, rcx, ctx::Rbx);
    a.load32(rbp, rcx, ctx::Rbp);
    a.load32(rsi, rcx, ctx::Rsi);
    a.load32(rdi, rcx, ctx::Rdi);
    a.load32(rcx, rcx, ctx::Rcx);
    a.far_return_to_guest(r11, r10);
    return a.b;
}

std::vector<uint8_t> emit_unwind64(uint32_t exit64)
{
    X64 a;
    a.mov_rr64(r15, rcx);
    a.load64(rax, r15, kBaseRsp);
    a.store64(r15, kHostRsp, rax);
    a.mov_rr32(rax, rdx);
    a.shr64(rdx, 32);
    a.mov_imm64(r11, exit64);
    a.jmp(r11);
    return a.b;
}

std::vector<uint8_t> emit_id_stub(uint16_t id, uint32_t ptr_full_gate64)
{
    X64 a;
    a.byte(0xB8);   // mov eax, id: eax is dead at every Xbox call boundary
    a.u32(id);
    a.bytes({0xFF, 0x2D});
    a.u32(ptr_full_gate64);
    while (a.b.size() < kIdStubSize) a.byte(0xCC);
    return a.b;
}

// The guest side of exception dispatch, 32-bit code entered with [esp] = &EXCEPTION_RECORD, [esp+4] = &CONTEXT (Xbox
// layout, FloatSave 16-aligned), [esp+8] = the KPCR and [esp+12] = the highest address a registration may reach (the
// stack base, or where a host call's nested guest call began: the registrations above belong to the code that made
// the host call, and their handlers would jump out of it). It walks fs:[0] as the Xbox kernel's RtlDispatchException
// does: each registration that climbs and lies in those bounds gets handler(record, registration, context,
// &dispatcher context), cdecl, behind a registration of the dispatcher's own (`nested`), as RtlpExecuteHandler2 puts
// one there: the C++ runtime's unwinder relinks the head it finds, and an exception inside a handler is told it is
// nested and skips the frames whose handler is running. ContinueSearch moves on; NestedException moves past the frame
// the dispatcher context names; ContinueExecution loads the CONTEXT, which the handler may have changed, and resumes
// there. The end of the chain, a bad registration, any other disposition or a noncontinuable exception jumps to
// `unhandled` with esp back at the arguments; the trap router turns its ud2 into the stop the exception would have been.
std::vector<uint8_t> emit_seh_dispatch(uint32_t nested, uint32_t unhandled)
{
    X64 a;
    std::vector<size_t> to_fail;
    auto jcc_fail = [&](uint8_t cc) {
        a.bytes({0x0F, cc});
        to_fail.push_back(a.b.size());
        a.u32(0);
    };
    auto rel8_here = [&](size_t at) { a.b[at] = uint8_t(a.b.size() - (at + 1)); };
    a.bytes({0x8B, 0x1C, 0x24});         // mov ebx, [esp]        record
    a.bytes({0x8B, 0x74, 0x24, 0x04});   // mov esi, [esp+4]      context
    a.bytes({0x8B, 0x7C, 0x24, 0x08});   // mov edi, [esp+8]      KPCR
    a.bytes({0x6A, 0x00});               // push 0                [esp+4]: dispatcher context
    a.bytes({0x6A, 0x00});               // push 0                [esp]: the last registration walked
    a.bytes({0x8B, 0x2F});               // mov ebp, [edi]        ExceptionList
    size_t next = a.b.size();
    a.bytes({0x83, 0xFD, 0xFF});         // cmp ebp, -1
    jcc_fail(0x84);                      // je
    a.bytes({0xF7, 0xC5, 3, 0, 0, 0});   // test ebp, 3
    jcc_fail(0x85);                      // jne
    a.bytes({0x3B, 0x2C, 0x24});         // cmp ebp, [esp]        climbing (0 is never above the last)
    jcc_fail(0x86);                      // jbe
    a.bytes({0x3B, 0x6F, 0x08});         // cmp ebp, [edi+8]      StackLimit
    jcc_fail(0x82);                      // jb
    a.bytes({0x8D, 0x45, 0x08});         // lea eax, [ebp+8]
    a.bytes({0x3B, 0x44, 0x24, 0x14});   // cmp eax, [esp+20]     the bound
    jcc_fail(0x87);                      // ja
    a.bytes({0x89, 0x2C, 0x24});         // mov [esp], ebp
    a.byte(0x55);                        // push ebp              nested registration: establisher
    a.byte(0x68);                        // push nested           handler
    a.u32(nested);
    a.bytes({0xFF, 0x37});               // push dword [edi]      next
    a.bytes({0x89, 0x27});               // mov [edi], esp
    a.bytes({0x8D, 0x44, 0x24, 0x10});   // lea eax, [esp+16]     &dispatcher context
    a.bytes({0x50, 0x56, 0x55, 0x53});   // push eax; push esi; push ebp; push ebx
    a.bytes({0xFF, 0x55, 0x04});         // call [ebp+4]
    a.bytes({0x83, 0xC4, 0x10});         // add esp, 16
    a.bytes({0x8B, 0x0C, 0x24});         // mov ecx, [esp]
    a.bytes({0x89, 0x0F});               // mov [edi], ecx        unlink the nested registration
    a.bytes({0x83, 0xC4, 0x0C});         // add esp, 12
    a.bytes({0x85, 0xC0});               // test eax, eax         ExceptionContinueExecution
    a.bytes({0x74, 0x00});               // je resume
    size_t to_resume = a.b.size() - 1;
    a.bytes({0x83, 0xF8, 0x01});         // cmp eax, 1            ExceptionContinueSearch
    a.bytes({0x74, 0x00});               // je search_on
    size_t to_search_on = a.b.size() - 1;
    a.bytes({0x83, 0xF8, 0x02});         // cmp eax, 2            ExceptionNestedException
    jcc_fail(0x85);                      // jne
    a.bytes({0x83, 0x4B, 0x04, 0x10});   // or dword [ebx+4], EXCEPTION_NESTED_CALL
    a.bytes({0x8B, 0x6C, 0x24, 0x04});   // mov ebp, [esp+4]      the frame whose handler is running
    a.bytes({0x89, 0x2C, 0x24});         // mov [esp], ebp
    rel8_here(to_search_on);
    a.bytes({0x8B, 0x6D, 0x00});         // mov ebp, [ebp]
    a.byte(0xE9);                        // jmp next
    a.u32(uint32_t(int32_t(next) - int32_t(a.b.size() + 4)));
    rel8_here(to_resume);
    a.bytes({0xF6, 0x43, 0x04, 0x01});   // test byte [ebx+4], EXCEPTION_NONCONTINUABLE
    jcc_fail(0x85);                      // jne
    a.bytes({0x0F, 0xAE, 0x4E, 0x04});   // fxrstor [esi+4]
    a.bytes({0x8B, 0x86}); a.u32(offsetof(XboxContext, Esp));      // mov eax, [esi+Esp]
    a.bytes({0x83, 0xE8, 0x08});                                   // sub eax, 8
    a.bytes({0x8B, 0x8E}); a.u32(offsetof(XboxContext, EFlags));   // mov ecx, [esi+EFlags]
    a.bytes({0x89, 0x08});                                         // mov [eax], ecx
    a.bytes({0x8B, 0x8E}); a.u32(offsetof(XboxContext, Eip));      // mov ecx, [esi+Eip]
    a.bytes({0x89, 0x48, 0x04});                                   // mov [eax+4], ecx
    a.bytes({0x89, 0xC4});                                         // mov esp, eax
    a.bytes({0x8B, 0xBE}); a.u32(offsetof(XboxContext, Edi));      // mov edi, [esi+Edi]
    a.bytes({0x8B, 0x9E}); a.u32(offsetof(XboxContext, Ebx));      // mov ebx, [esi+Ebx]
    a.bytes({0x8B, 0x96}); a.u32(offsetof(XboxContext, Edx));      // mov edx, [esi+Edx]
    a.bytes({0x8B, 0x8E}); a.u32(offsetof(XboxContext, Ecx));      // mov ecx, [esi+Ecx]
    a.bytes({0x8B, 0x86}); a.u32(offsetof(XboxContext, Eax));      // mov eax, [esi+Eax]
    a.bytes({0x8B, 0xAE}); a.u32(offsetof(XboxContext, Ebp));      // mov ebp, [esi+Ebp]
    a.bytes({0x8B, 0xB6}); a.u32(offsetof(XboxContext, Esi));      // mov esi, [esi+Esi]
    a.byte(0x9D);                                                  // popfd
    a.byte(0xC3);                                                  // ret
    size_t fail = a.b.size();
    a.bytes({0x83, 0xC4, 0x08});         // add esp, 8            back at the arguments
    a.byte(0xB8);                        // mov eax, unhandled
    a.u32(unhandled);
    a.bytes({0xFF, 0xE0});               // jmp eax
    for (size_t at : to_fail) {
        uint32_t rel = uint32_t(int32_t(fail) - int32_t(at + 4));
        for (int i = 0; i < 4; i++) a.b[at + i] = uint8_t(rel >> (8 * i));
    }
    return a.b;
}

// The handler of the registration the dispatcher puts around each handler it calls (RtlpExceptionHandler): an
// unwind passes it by; an exception raised while that handler runs is nested, and the dispatcher context names the
// frame being handled so the nested dispatch searches past it.
std::vector<uint8_t> emit_seh_nested()
{
    return {
        0x8B, 0x4C, 0x24, 0x04,               // mov ecx, [esp+4]      record
        0xB8, 0x01, 0x00, 0x00, 0x00,         // mov eax, 1            ExceptionContinueSearch
        0xF7, 0x41, 0x04, 0x06, 0, 0, 0,      // test dword [ecx+4], EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND
        0x75, 0x0F,                           // jne done
        0x8B, 0x4C, 0x24, 0x08,               // mov ecx, [esp+8]      this registration
        0x8B, 0x54, 0x24, 0x10,               // mov edx, [esp+16]     &dispatcher context
        0x8B, 0x49, 0x08,                     // mov ecx, [ecx+8]      its establisher
        0x89, 0x0A,                           // mov [edx], ecx
        0xB0, 0x02,                           // mov al, 2             ExceptionNestedException
        0xC3,                                 // done: ret
    };
}

std::vector<uint8_t> emit_seh_unhandled() { return {0x0F, 0x0B}; }   // ud2

std::vector<uint8_t> emit_dm_notify() { return {0x33, 0xC0, 0xC2, 0x08, 0x00}; }   // xor eax,eax; ret 8

std::vector<uint8_t> emit_fault_gate()
{
    X64 a;
    a.mov_rr32(rsp, rsp);   // zero-extend rsp as every gate does, so the kernel can build the trap frame
    a.ud2();
    return a.b;
}

std::vector<uint8_t> emit_far_pointer(uint32_t offset, uint16_t selector)
{
    X64 a;
    a.u32(offset);
    a.bytes({uint8_t(selector), uint8_t(selector >> 8), 0, 0});
    return a.b;
}

std::vector<uint8_t> emit_patch(uint32_t ptr_variant, uint8_t length)
{
    X64 a;
    a.bytes({0xFF, 0x1D});   // call far [ptr]
    a.u32(ptr_variant);
    while (a.b.size() < length) a.byte(0x90);
    return a.b;
}

bool has_micro_gate(SiteKind kind, uint8_t reg)
{
    switch (kind) {
    case SiteKind::FsLoad:
    case SiteKind::FsStore:
    case SiteKind::FsMovzxByte:
    case SiteKind::FsCmp:
    case SiteKind::FsCmpRev: return reg != rsp;   // esp inside the gate is the guest's minus the far-call frame
    case SiteKind::FsStoreEsp:
    case SiteKind::FsStoreImm:
    case SiteKind::FsPush:
    case SiteKind::FsPop: return true;
    case SiteKind::FsOther:
    case SiteKind::Rdtsc:
    case SiteKind::Privileged: return false;
    }
    return false;
}

// On entry: [rsp] = eip, [rsp+4] = 0x23 from the far call; the guest's esp is rsp+8.
std::vector<uint8_t> emit_micro_gate(SiteKind kind, uint8_t reg, uint32_t disp, uint32_t imm)
{
    if (!has_micro_gate(kind, reg)) return {};
    X64 a;
    int32_t d = int32_t(disp);
    a.mov_rr32(rsp, rsp);
    switch (kind) {
    case SiteKind::FsLoad: a.load32(reg, r15, d); break;
    case SiteKind::FsStore: a.store32(r15, d, reg); break;
    case SiteKind::FsStoreEsp:
        a.lea32(r11, rsp, 8);
        a.store32(r15, d, r11);
        break;
    case SiteKind::FsStoreImm: a.store_imm32(r15, d, imm); break;
    case SiteKind::FsPush:
        a.load32(r10, rsp, 0);
        a.load32(r11, r15, d);
        a.store32(rsp, 4, r11);
        a.store32(rsp, -4, r10);
        a.store_imm32(rsp, 0, kCompatCs);
        a.lea64(rsp, rsp, -4);
        break;
    case SiteKind::FsPop:
        a.load32(r11, rsp, 8);
        a.store32(r15, d, r11);
        a.load32(r10, rsp, 0);
        a.store32(rsp, 4, r10);
        a.store_imm32(rsp, 8, kCompatCs);
        a.lea64(rsp, rsp, 4);
        break;
    case SiteKind::FsMovzxByte: a.movzx8(reg, r15, d); break;
    case SiteKind::FsCmp: a.cmp_rm(reg, r15, d); break;
    case SiteKind::FsCmpRev: a.cmp_mr(r15, d, reg); break;
    case SiteKind::FsOther:
    case SiteKind::Rdtsc:
    case SiteKind::Privileged: return {};
    }
    a.retf();
    return a.b;
}

}  // namespace xbrt::detail
