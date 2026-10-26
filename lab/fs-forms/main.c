#include "../lab.h"

#define ARBITRARY_USER_POINTER 0x14
#define NOINLINE __attribute__((noinline))

typedef ULONG (*Probe)(volatile ULONG *kpcr);

static NOINLINE ULONG load_a1(volatile ULONG *kpcr)
{
    kpcr[ARBITRARY_USER_POINTER / 4] = 0x11112222;
    ULONG v;
    __asm__ volatile(".byte 0x64, 0xA1, 0x14, 0x00, 0x00, 0x00" : "=a"(v)::"memory");   // mov eax,fs:[0x14]
    return v;
}

static NOINLINE ULONG load_8b(volatile ULONG *kpcr)
{
    kpcr[ARBITRARY_USER_POINTER / 4] = 0x33334444;
    ULONG v;
    __asm__ volatile(".byte 0x64, 0x8B, 0x15, 0x14, 0x00, 0x00, 0x00" : "=d"(v)::"memory");   // mov edx,fs:[0x14]
    return v;
}

static NOINLINE ULONG store_reg(volatile ULONG *kpcr)
{
    __asm__ volatile(".byte 0x64, 0x89, 0x35, 0x14, 0x00, 0x00, 0x00" ::"S"(0x55556666u) : "memory");   // mov fs:[0x14],esi
    return kpcr[ARBITRARY_USER_POINTER / 4];
}

static NOINLINE ULONG store_esp_to_exception_list(volatile ULONG *kpcr)
{
    ULONG saved = kpcr[0], esp;
    __asm__ volatile("movl %%esp, %0\n"
                     ".byte 0x64, 0x89, 0x25, 0x00, 0x00, 0x00, 0x00"   // mov fs:[0],esp
                     : "=r"(esp)::"memory");
    ULONG stored = kpcr[0];
    kpcr[0] = saved;
    return stored - esp;
}

static NOINLINE ULONG store_imm(volatile ULONG *kpcr)
{
    __asm__ volatile(".byte 0x64, 0xC7, 0x05, 0x14, 0x00, 0x00, 0x00, 0x88, 0x88, 0x77, 0x77" ::: "memory");   // mov dword fs:[0x14],0x77778888
    return kpcr[ARBITRARY_USER_POINTER / 4];
}

static NOINLINE ULONG push_value(volatile ULONG *kpcr)
{
    kpcr[ARBITRARY_USER_POINTER / 4] = 0x9999AAAA;
    ULONG v;
    __asm__ volatile(".byte 0x64, 0xFF, 0x35, 0x14, 0x00, 0x00, 0x00\n"   // push dword fs:[0x14]
                     "popl %0"
                     : "=r"(v)::"memory");
    return v;
}

static NOINLINE ULONG push_esp_delta(volatile ULONG *kpcr)
{
    ULONG delta;
    __asm__ volatile("movl %%esp, %%ecx\n"
                     ".byte 0x64, 0xFF, 0x35, 0x14, 0x00, 0x00, 0x00\n"   // push dword fs:[0x14]
                     "movl %%esp, %0\n"
                     "addl $4, %%esp\n"
                     "subl %%ecx, %0"
                     : "=r"(delta)::"ecx", "memory");
    return delta;
}

static NOINLINE ULONG pop_value(volatile ULONG *kpcr)
{
    __asm__ volatile("pushl %0\n"
                     ".byte 0x64, 0x8F, 0x05, 0x14, 0x00, 0x00, 0x00"   // pop dword fs:[0x14]
                     ::"r"(0xBBBBCCCCu) : "memory");
    return kpcr[ARBITRARY_USER_POINTER / 4];
}

static NOINLINE ULONG pop_esp_delta(volatile ULONG *kpcr)
{
    ULONG delta;
    __asm__ volatile("pushl $0\n"
                     "movl %%esp, %%ecx\n"
                     ".byte 0x64, 0x8F, 0x05, 0x14, 0x00, 0x00, 0x00\n"   // pop dword fs:[0x14]
                     "movl %%esp, %0\n"
                     "subl %%ecx, %0"
                     : "=r"(delta)::"ecx", "memory");
    return delta;
}

static NOINLINE ULONG movzx_scratch(volatile ULONG *kpcr)
{
    kpcr[ARBITRARY_USER_POINTER / 4] = 0xDDDDDDA5;
    ULONG v;
    __asm__ volatile(".byte 0x64, 0x0F, 0xB6, 0x05, 0x14, 0x00, 0x00, 0x00" : "=a"(v)::"memory");   // movzx eax,byte fs:[0x14]
    return v;
}

static NOINLINE ULONG movzx_irql(volatile ULONG *kpcr)
{
    ULONG v;
    __asm__ volatile(".byte 0x64, 0x0F, 0xB6, 0x0D, 0x24, 0x00, 0x00, 0x00" : "=c"(v)::"memory");   // movzx ecx,byte fs:[0x24]
    return v;
}

static NOINLINE ULONG cmp(volatile ULONG *kpcr, ULONG esi)
{
    kpcr[ARBITRARY_USER_POINTER / 4] = 0x5EED5EED;
    ULONG v;
    __asm__ volatile("xorl %%eax, %%eax\n"
                     ".byte 0x64, 0x3B, 0x35, 0x14, 0x00, 0x00, 0x00\n"   // cmp esi,fs:[0x14]
                     "setz %%al\n"
                     "setb %%ah"
                     : "=&a"(v)
                     : "S"(esi)
                     : "cc", "memory");
    return v;
}

static NOINLINE ULONG cmp_rev(volatile ULONG *kpcr, ULONG esi)
{
    kpcr[ARBITRARY_USER_POINTER / 4] = 0x5EED5EED;
    ULONG v;
    __asm__ volatile("xorl %%eax, %%eax\n"
                     ".byte 0x64, 0x39, 0x35, 0x14, 0x00, 0x00, 0x00\n"   // cmp fs:[0x14],esi
                     "setz %%al\n"
                     "setb %%ah"
                     : "=&a"(v)
                     : "S"(esi)
                     : "cc", "memory");
    return v;
}

static ULONG cmp_equal(volatile ULONG *kpcr) { return cmp(kpcr, 0x5EED5EED); }
static ULONG cmp_below(volatile ULONG *kpcr) { return cmp(kpcr, 1); }
static ULONG cmp_rev_equal(volatile ULONG *kpcr) { return cmp_rev(kpcr, 0x5EED5EED); }
static ULONG cmp_rev_above(volatile ULONG *kpcr) { return cmp_rev(kpcr, 1); }

static NOINLINE ULONG add_unpromotable(volatile ULONG *kpcr)
{
    kpcr[ARBITRARY_USER_POINTER / 4] = 0x5EED5EED;
    ULONG v = 0x11111111;
    __asm__ volatile(".byte 0x64, 0x03, 0x05, 0x14, 0x00, 0x00, 0x00" : "+a"(v)::"cc", "memory");   // add eax,fs:[0x14]
    return v;
}

static const struct {
    const char *name;
    Probe run;
} kProbes[] = {
    {"FsLoad-A1", load_a1},
    {"FsLoad-8B", load_8b},
    {"FsStore", store_reg},
    {"FsStore-esp", store_esp_to_exception_list},
    {"FsStoreImm", store_imm},
    {"FsPush", push_value},
    {"FsPush-esp", push_esp_delta},
    {"FsPop", pop_value},
    {"FsPop-esp", pop_esp_delta},
    {"FsMovzxByte", movzx_scratch},
    {"FsMovzxByte-irql", movzx_irql},
    {"FsCmp-equal", cmp_equal},
    {"FsCmp-below", cmp_below},
    {"FsCmpRev-equal", cmp_rev_equal},
    {"FsCmpRev-above", cmp_rev_above},
    {"unpromotable-add", add_unpromotable},
};
#define PROBES (sizeof kProbes / sizeof kProbes[0])

volatile ULONG lab_fs_results[2][PROBES];

static NOINLINE void run_probes(volatile ULONG *kpcr, int run)
{
    for (unsigned i = 0; i < PROBES; i++) lab_fs_results[run][i] = kProbes[i].run(kpcr);
}

void __cdecl lab_entry(void)
{
    DbgPrint("lab fs-forms start");

    ULONG self_pcr, self_tib, prcb;
    __asm__ volatile("movl %%fs:0x1C, %0" : "=r"(self_pcr));
    __asm__ volatile("movl %%fs:0x18, %0" : "=r"(self_tib));
    __asm__ volatile("movl %%fs:0x20, %0" : "=r"(prcb));
    if (self_pcr < 0x10000 || self_tib != self_pcr || prcb != self_pcr + 0x28) {
        LabLine s = lab_step("KPCR fs:[0x1C] SelfPcr ");
        lab_put_hex(&s, self_pcr);
        lab_put(&s, " fs:[0x18] Self ");
        lab_put_hex(&s, self_tib);
        lab_put(&s, " fs:[0x20] Prcb ");
        lab_put_hex(&s, prcb);
        lab_finish("fs-forms", &s);
    }
    volatile ULONG *kpcr = (volatile ULONG *)self_pcr;
    ULONG scratch = kpcr[ARBITRARY_USER_POINTER / 4];

    run_probes(kpcr, 0);
    run_probes(kpcr, 1);
    kpcr[ARBITRARY_USER_POINTER / 4] = scratch;

    for (unsigned i = 0; i < PROBES; i++) {
        LabLine l = lab_step("lab fs-forms ");
        lab_put(&l, kProbes[i].name);
        lab_put(&l, " ");
        lab_put_hex(&l, lab_fs_results[0][i]);
        lab_put(&l, " ");
        lab_put_hex(&l, lab_fs_results[1][i]);
        lab_print(&l);
    }
    lab_finish("fs-forms", NULL);
}
