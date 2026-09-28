#include <jam/cpu.h>
#include <jam/string.h>
#include <jam/x86.h>

struct cpu_features cpu_features;

void cpu_detect(void)
{
    uint32_t a, b, c, d;
    struct cpu_features *f = &cpu_features;

    cpuid(0, 0, &a, &b, &c, &d);
    uint32_t max_leaf = a;
    memcpy(f->vendor + 0, &b, 4);
    memcpy(f->vendor + 4, &d, 4);
    memcpy(f->vendor + 8, &c, 4);
    f->vendor[12] = '\0';

    cpuid(1, 0, &a, &b, &c, &d);
    f->pge    = d & (1u << 13);
    f->pat    = d & (1u << 16);
    f->x2apic = c & (1u << 21);

    if (max_leaf >= 7) {
        cpuid(7, 0, &a, &b, &c, &d);
        f->hybrid = d & (1u << 15);
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
