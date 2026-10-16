/* /proc/cpuinfo (docs/CR-PLAN.md "PRC notes"): Linux's x86 text, one block
 * per CPU online, made from CPUID as the CPU we run on answers it (the
 * CPUs of one machine are alike, bar a hybrid part's, which this does not
 * tell apart) and from the figures any process may read (CPUs online:
 * sys_public_info; the TSC's rate: the time page).
 *
 * Real: vendor_id, cpu family, model, model name, stepping, cpu MHz (the
 * TSC's rate: Jam OS has no frequency scaling to report), cache size (the
 * largest cache CPUID names), cpuid level, flags (Linux's names for the
 * feature bits set), bogomips (twice the MHz, as Linux's calibration
 * comes out), clflush size, cache_alignment, address sizes.
 * Neutral: microcode 0x0 (an MSR we can't read), physical id 0, siblings
 * and cpu cores the CPUs online (no SMT is assumed), core id, apicid and
 * initial apicid the CPU's number (a CPU's APIC id needs a root
 * handle's cpu_stat), bugs empty, power management empty. */
#include <stdio.h>
#include <string.h>
#include "layer.h"
#include "procinfo.h"

/* Where a flag's bit is: a register of one CPUID leaf. */
enum src { S_1D, S_1C, S_7B, S_7C, S_7D, S_X1D, S_X1C, S_D1A, S_X7D, S_COUNT };

struct flag {
    uint8_t     src;
    uint8_t     bit;
    const char *name;
};

/* Linux's names (arch/x86/include/asm/cpufeatures.h and its /proc names). */
static const struct flag flags[] = {
    { S_1D, 0, "fpu" }, { S_1D, 1, "vme" }, { S_1D, 2, "de" }, { S_1D, 3, "pse" },
    { S_1D, 4, "tsc" }, { S_1D, 5, "msr" }, { S_1D, 6, "pae" }, { S_1D, 7, "mce" },
    { S_1D, 8, "cx8" }, { S_1D, 9, "apic" }, { S_1D, 11, "sep" }, { S_1D, 12, "mtrr" },
    { S_1D, 13, "pge" }, { S_1D, 14, "mca" }, { S_1D, 15, "cmov" }, { S_1D, 16, "pat" },
    { S_1D, 17, "pse36" }, { S_1D, 19, "clflush" }, { S_1D, 23, "mmx" }, { S_1D, 24, "fxsr" },
    { S_1D, 25, "sse" }, { S_1D, 26, "sse2" }, { S_1D, 28, "ht" },
    { S_X1D, 11, "syscall" }, { S_X1D, 20, "nx" }, { S_X1D, 26, "pdpe1gb" },
    { S_X1D, 27, "rdtscp" }, { S_X1D, 29, "lm" },
    { S_X7D, 8, "constant_tsc" }, { S_X7D, 8, "nonstop_tsc" },
    { S_1C, 0, "pni" }, { S_1C, 1, "pclmulqdq" }, { S_1C, 3, "monitor" }, { S_1C, 5, "vmx" },
    { S_1C, 7, "est" }, { S_1C, 9, "ssse3" }, { S_1C, 12, "fma" }, { S_1C, 13, "cx16" },
    { S_1C, 15, "pdcm" }, { S_1C, 17, "pcid" }, { S_1C, 19, "sse4_1" }, { S_1C, 20, "sse4_2" },
    { S_1C, 21, "x2apic" }, { S_1C, 22, "movbe" }, { S_1C, 23, "popcnt" },
    { S_1C, 24, "tsc_deadline_timer" }, { S_1C, 25, "aes" }, { S_1C, 26, "xsave" },
    { S_1C, 28, "avx" }, { S_1C, 29, "f16c" }, { S_1C, 30, "rdrand" },
    { S_1C, 31, "hypervisor" },
    { S_X1C, 0, "lahf_lm" }, { S_X1C, 5, "abm" },
    { S_D1A, 0, "xsaveopt" }, { S_D1A, 1, "xsavec" }, { S_D1A, 3, "xsaves" },
    { S_7B, 0, "fsgsbase" }, { S_7B, 3, "bmi1" }, { S_7B, 4, "hle" }, { S_7B, 5, "avx2" },
    { S_7B, 7, "smep" }, { S_7B, 8, "bmi2" }, { S_7B, 9, "erms" }, { S_7B, 10, "invpcid" },
    { S_7B, 11, "rtm" }, { S_7B, 16, "avx512f" }, { S_7B, 17, "avx512dq" },
    { S_7B, 18, "rdseed" }, { S_7B, 19, "adx" }, { S_7B, 20, "smap" },
    { S_7B, 23, "clflushopt" }, { S_7B, 24, "clwb" }, { S_7B, 28, "avx512cd" },
    { S_7B, 29, "sha_ni" }, { S_7B, 30, "avx512bw" }, { S_7B, 31, "avx512vl" },
    { S_7C, 1, "avx512vbmi" }, { S_7C, 2, "umip" }, { S_7C, 3, "pku" }, { S_7C, 8, "gfni" },
    { S_7C, 9, "vaes" }, { S_7C, 10, "vpclmulqdq" }, { S_7C, 11, "avx512_vnni" },
    { S_7C, 14, "avx512_vpopcntdq" }, { S_7C, 22, "rdpid" },
    { S_7D, 4, "fsrm" },
};

/* The feature registers of the CPU we run on; 0 for a leaf it doesn't have. */
struct regs {
    uint32_t src[S_COUNT];
    uint32_t max_basic, max_ext;
    char     vendor[13];
    char     brand[49];
    uint32_t fms;         /* leaf 1 EAX */
    uint32_t clflush;     /* leaf 1 EBX bits 15:8, in 8-byte units */
    uint32_t addr_bits;   /* leaf 0x80000008 EAX */
};

static void read_regs(struct regs *g)
{
    uint32_t r[4];
    memset(g, 0, sizeof(*g));
    cpu_cpuid(0, 0, r);
    g->max_basic = r[0];
    memcpy(g->vendor, &r[1], 4);
    memcpy(g->vendor + 4, &r[3], 4);
    memcpy(g->vendor + 8, &r[2], 4);
    cpu_cpuid(0x80000000u, 0, r);
    g->max_ext = r[0] >= 0x80000000u ? r[0] : 0;
    if (g->max_basic >= 1) {
        cpu_cpuid(1, 0, r);
        g->fms = r[0];
        g->clflush = (r[1] >> 8) & 0xff;
        g->src[S_1C] = r[2];
        g->src[S_1D] = r[3];
    }
    if (g->max_basic >= 7) {
        cpu_cpuid(7, 0, r);
        g->src[S_7B] = r[1];
        g->src[S_7C] = r[2];
        g->src[S_7D] = r[3];
    }
    if (g->max_basic >= 0xd) {
        cpu_cpuid(0xd, 1, r);
        g->src[S_D1A] = r[0];
    }
    if (g->max_ext >= 0x80000001u) {
        cpu_cpuid(0x80000001u, 0, r);
        g->src[S_X1C] = r[2];
        g->src[S_X1D] = r[3];
    }
    if (g->max_ext >= 0x80000007u) {
        cpu_cpuid(0x80000007u, 0, r);
        g->src[S_X7D] = r[3];
    }
    if (g->max_ext >= 0x80000008u) {
        cpu_cpuid(0x80000008u, 0, r);
        g->addr_bits = r[0];
    }
    if (g->max_ext >= 0x80000004u) {
        for (uint32_t i = 0; i < 3; i++) {
            cpu_cpuid(0x80000002u + i, 0, r);
            memcpy(g->brand + 16 * i, r, 16);
        }
    }
}

/* The brand string without CPUID's padding and its leading spaces. */
static const char *brand_of(struct regs *g)
{
    char *b = g->brand;
    size_t n = strnlen(b, sizeof(g->brand) - 1);
    while (n && b[n - 1] == ' ')
        b[--n] = '\0';
    while (*b == ' ')
        b++;
    return *b ? b : "unknown";
}

/* The biggest cache CPUID names, in KiB (0: it names none). */
static unsigned cache_kb(const struct regs *g)
{
    unsigned best = 0, best_level = 0;
    uint32_t r[4];
    if (g->max_basic >= 4) {   /* Intel's leaf, and AMD's equivalent below */
        for (uint32_t i = 0; i < 32; i++) {
            cpu_cpuid(4, i, r);
            unsigned type = r[0] & 31, level = (r[0] >> 5) & 7;
            if (!type)
                break;
            if (type == 2)   /* instructions only */
                continue;
            unsigned kb = (unsigned)(((uint64_t)((r[1] >> 22) + 1) * (((r[1] >> 12) & 0x3ff) + 1) *
                                      ((r[1] & 0xfff) + 1) * ((uint64_t)r[2] + 1)) >> 10);
            if (level >= best_level) {
                best_level = level;
                best = kb;
            }
        }
    }
    if (!best && g->max_ext >= 0x80000006u) {   /* AMD: L3 in 512 KiB units, else L2 in KiB */
        cpu_cpuid(0x80000006u, 0, r);
        best = (r[3] >> 18) ? (r[3] >> 18) * 512 : r[2] >> 16;
    }
    return best;
}

/* The flags line's names, in the table's order. */
static void put_flags(struct sb *b, const struct regs *g)
{
    const char *sep = "";
    for (unsigned i = 0; i < sizeof(flags) / sizeof(flags[0]); i++)
        if (g->src[flags[i].src] >> flags[i].bit & 1) {
            sb_put(b, "%s%s", sep, flags[i].name);
            sep = " ";
        }
}

void cpuinfo_text(struct sb *b)
{
    struct regs g;
    struct kp_public pub;
    read_regs(&g);
    unsigned cpus = kp_public_info(&pub) == OK && pub.cpus ? pub.cpus : 1;
    unsigned family = (g.fms >> 8) & 0xf, model = (g.fms >> 4) & 0xf, step = g.fms & 0xf;
    if (family == 0xf)
        family += (g.fms >> 20) & 0xff;
    if ((g.fms >> 8 & 0xf) == 0x6 || (g.fms >> 8 & 0xf) == 0xf)
        model += ((g.fms >> 16) & 0xf) << 4;
    uint64_t hz = time_tsc_hz();
    unsigned mhz_milli = (unsigned)(hz / 1000);   /* thousandths of a MHz */
    unsigned kb = cache_kb(&g);
    const char *brand = brand_of(&g);
    for (unsigned i = 0; i < cpus; i++) {
        sb_put(b, "processor\t: %u\n", i);
        sb_put(b, "vendor_id\t: %s\n", g.vendor[0] ? g.vendor : "unknown");
        sb_put(b, "cpu family\t: %u\n", family);
        sb_put(b, "model\t\t: %u\n", model);
        sb_put(b, "model name\t: %s\n", brand);
        sb_put(b, "stepping\t: %u\n", step);
        sb_put(b, "microcode\t: 0x0\n");
        sb_put(b, "cpu MHz\t\t: %u.%03u\n", mhz_milli / 1000, mhz_milli % 1000);
        sb_put(b, "cache size\t: %u KB\n", kb);
        sb_put(b, "physical id\t: 0\n");
        sb_put(b, "siblings\t: %u\n", cpus);
        sb_put(b, "core id\t\t: %u\n", i);
        sb_put(b, "cpu cores\t: %u\n", cpus);
        sb_put(b, "apicid\t\t: %u\n", i);
        sb_put(b, "initial apicid\t: %u\n", i);
        sb_put(b, "fpu\t\t: yes\n");
        sb_put(b, "fpu_exception\t: yes\n");
        sb_put(b, "cpuid level\t: %u\n", g.max_basic);
        sb_put(b, "wp\t\t: yes\n");
        sb_put(b, "flags\t\t: ");
        put_flags(b, &g);
        sb_put(b, "\n");
        sb_put(b, "bugs\t\t:\n");
        sb_put(b, "bogomips\t: %u.%02u\n", mhz_milli * 2 / 1000, mhz_milli * 2 % 1000 / 10);
        sb_put(b, "clflush size\t: %u\n", g.clflush * 8);
        sb_put(b, "cache_alignment\t: %u\n", g.clflush * 8);
        sb_put(b, "address sizes\t: %u bits physical, %u bits virtual\n", g.addr_bits & 0xff,
               (g.addr_bits >> 8) & 0xff);
        sb_put(b, "power management:\n\n");
    }
}
