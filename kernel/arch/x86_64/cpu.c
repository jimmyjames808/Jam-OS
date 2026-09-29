#include <jam/cpu.h>
#include <jam/pcid.h>
#include <jam/percpu.h>
#include <jam/string.h>
#include <jam/x86.h>

struct cpu_features cpu_features;
/* Read by the entry assembly (STAC_IF_SMAP / CLAC_IF_SMAP): stac and clac
 * exist only on CPUs with SMAP. */
uint8_t smap_on;

void cpu_detect(void)
{
    uint32_t a, b, c, d;
    struct cpu_features *f = &cpu_features;

    cpuid(0, 0, &a, &b, &c, &d);
    uint32_t max_leaf = a;
    f->max_leaf = max_leaf;
    memcpy(f->vendor + 0, &b, 4);
    memcpy(f->vendor + 4, &d, 4);
    memcpy(f->vendor + 8, &c, 4);
    f->vendor[12] = '\0';

    cpuid(1, 0, &a, &b, &c, &d);
    f->pge    = d & (1u << 13);
    f->pat    = d & (1u << 16);
    f->x2apic = c & (1u << 21);
    f->tsc_deadline = c & (1u << 24);
    f->xsave  = c & (1u << 26);
    f->avx    = f->xsave && (c & (1u << 28));
    f->pcid   = c & (1u << 17);

    if (max_leaf >= 7) {
        cpuid(7, 0, &a, &b, &c, &d);
        f->hybrid = d & (1u << 15);
        f->smep   = b & (1u << 7);
        f->smap   = b & (1u << 20);
        f->umip   = c & (1u << 2);
        f->invpcid = b & (1u << 10);
    }
    smap_on = f->smap;

    if (max_leaf >= 0x15) {
        cpuid(0x15, 0, &a, &b, &c, &d);
        f->tsc_ratio_den = a;
        f->tsc_ratio_num = b;
        f->crystal_hz = c;
    }

    cpuid(0x80000000, 0, &a, &b, &c, &d);
    uint32_t max_ext = a;
    if (max_ext >= 0x80000001) {
        cpuid(0x80000001, 0, &a, &b, &c, &d);
        f->nx       = d & (1u << 20);
        f->pages_1g = d & (1u << 26);
    }
    if (max_ext >= 0x80000004) {
        uint32_t *brand = (uint32_t *)f->brand;
        for (uint32_t i = 0; i < 3; i++)
            cpuid(0x80000002 + i, 0, &brand[i * 4], &brand[i * 4 + 1],
                  &brand[i * 4 + 2], &brand[i * 4 + 3]);
        f->brand[48] = '\0';
    }
    if (max_ext >= 0x80000007) {
        cpuid(0x80000007, 0, &a, &b, &c, &d);
        f->tsc_invariant = d & (1u << 8);
    }
}

void cpu_enable_paging_features(void)
{
    if (cpu_features.nx)
        wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE);
    write_cr0(read_cr0() | CR0_WP);   /* kernel honours read-only pages */
    if (cpu_features.pge)
        write_cr4(read_cr4() | CR4_PGE);

    /* PA0 WB, PA1 WT, PA2 UC-, PA3 UC, PA4 WP, PA5 WC, PA6 UC-, PA7 UC.
     * Matches the power-on default for 0-3 and the layout Limine uses. */
    if (cpu_features.pat) {
        wbinvd();
        wrmsr(MSR_PAT, 0x0007010500070406ull);
        wbinvd();
    }
}

void fpu_init_cpu(void);
void syscall_init_cpu(void);

void cpu_init_local(void)
{
    uint64_t cr4 = read_cr4();
    /* SMEP: the kernel never executes a user page. SMAP: it never touches
     * one outside the user-copy routines (stac/clac). UMIP: user code can't
     * read the GDT/IDT/TSS addresses with sgdt and friends. */
    if (cpu_features.smep)
        cr4 |= CR4_SMEP;
    if (cpu_features.smap)
        cr4 |= CR4_SMAP;
    if (cpu_features.umip)
        cr4 |= CR4_UMIP;
    /* With FSGSBASE user code could set its GS base to a kernel-looking
     * address and fool the NMI/#MC/#DB entries, which tell the kernel's GS
     * from the user's by the base's sign. The loader may have left it on. */
    cr4 &= ~CR4_FSGSBASE;
    /* PCIDs (M5.5, pcid.c). Needs CR3's PCID bits to be 0 when it is set:
     * every CPU is on the kernel's tables here. */
    if (pcid_usable())
        cr4 |= CR4_PCIDE;
    write_cr4(cr4);
    fpu_init_cpu();
    syscall_init_cpu();
}

void cpu_detect_topology(struct cpu *cpu)
{
    uint32_t a, b, c, d;
    cpu->type = CORE_UNKNOWN;
    if (cpu_features.hybrid && cpu_features.max_leaf >= 0x1a) {
        cpuid(0x1a, 0, &a, &b, &c, &d);
        switch (a >> 24) {
        case 0x40: cpu->type = CORE_PERFORMANCE; break;   /* "Core" */
        case 0x20: cpu->type = CORE_EFFICIENCY; break;    /* "Atom" */
        }
    }

    /* Leaf 0x1F (or 0xB): sub-leaf 0 describes the SMT level. The shift says
     * how many low x2APIC-id bits pick a thread within a core. */
    uint32_t leaf = cpu_features.max_leaf >= 0x1f ? 0x1f : 0xb;
    uint32_t smt_shift = 0, x2id = cpu->lapic_id;
    if (cpu_features.max_leaf >= 0xb) {
        cpuid(leaf, 0, &a, &b, &c, &d);
        x2id = d;
        if (((c >> 8) & 0xff) == 1)
            smt_shift = a & 0x1f;
    }
    cpu->smt_id = x2id & ((1u << smt_shift) - 1);
    cpu->core_id = x2id >> smt_shift;
}
