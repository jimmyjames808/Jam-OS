#define MAP_32BIT      0x40

/* Jam OS: JIT memory (docs/CR-PLAN.md section 12). With MAP_PRIVATE |
 * MAP_ANONYMOUS, in a program whose list says `right jit` (approved), the
 * mapping is JIT memory: PROT_EXEC is allowed on it, with PROT_WRITE too,
 * in mmap and mprotect; without the power mmap fails with EPERM. macOS's
 * name, not its value (0x800 is Linux's MAP_DENYWRITE, which programs pass
 * and expect ignored): a bit Linux leaves free on x86-64, the top one below
 * the huge-page size field (MAP_HUGE_SHIFT, 26), farthest from where
 * Linux's own flags have been growing (up from MAP_FIXED_NOREPLACE). */
#define MAP_JIT        0x2000000
