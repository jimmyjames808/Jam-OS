/* kexec: starting a fresh copy of Jam OS without the firmware, after a
 * reboot or a panic (kernel/kexec/; the plan is docs/M8.5-PLAN.md,
 * "Revision 2").
 *
 * At boot the kernel reserves a physically contiguous region of RAM
 * (crashkernel=<MiB>, default 128, below 4 GiB) and unmaps it: the HHDM
 * skips it (BOOT_MEM_FOREIGN) and nothing else maps it, so no wild write
 * can reach it. Into it goes the stored kernel, a ready-to-run copy of
 * the kernel and bootfs this boot started from (the boot modules: Limine
 * loads jamos.elf a second time as one, and a kexec'd kernel is handed it
 * the same way): its segments, its bootfs, the kernel file again (so the
 * next kernel can store its own copy), the page tables it starts on and
 * the handoff it reads (<jam/kexec_handoff.h>), checksummed once written.
 * It is a normal boot in every way: every CPU, all of RAM but the region's
 * loaded parts and this kernel's crash record and log ring, which the
 * next kernel reads and then frees.
 *
 * Two ways in: kexec_reboot (init's `reboot`) and a panic
 * (kexec_panic_begin/_jump). kexec_load_image replaces the stored kernel
 * with another (init, when the files on /esp changed).
 *
 * State (one word, read by the panic path without a lock): OFF (no
 * region), EMPTY, LOADING, ARMED (a stored kernel), JUMPING. Every change
 * of it and every write to the region holds the kexec mutex; the panic
 * path takes nothing and jumps only from ARMED. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <jam/boot.h>
#include <jam/status.h>

#define KEXEC_DEFAULT_MIB 128
#define KEXEC_KERNEL_MODULE "jamos.elf"   /* the pristine kernel: a module path's suffix */
/* A panic this soon after a start that was itself a panic's halts on its
 * panic screen instead of jumping again (a crash loop). */
#define KEXEC_LOOP_NS (30ull * 1000000000ull)

struct vmo;

/* ---- the running kernel ------------------------------------------------------- */

/* kmain, before the memory managers: take the region out of bi's memory
 * map (it becomes BOOT_MEM_FOREIGN) unless the command line says
 * crashkernel=0. Logs what it did. */
void kexec_reserve(struct boot_info *bi);
/* kmain_stage2, with the heap up and the other CPUs started: load the
 * stored kernel from the boot modules and arm it. Logs what it did; a
 * failure leaves the region EMPTY (a panic then halts on its screen). */
void kexec_load_stored(void);
/* The region: base and size in bytes; false if there is none. */
bool kexec_region(uint64_t *base, uint64_t *size);
/* A stored kernel is armed (a panic or kexec_reboot would start it). */
bool kexec_armed(void);
/* The command line a stored kernel gets, from this boot's (`from`): only
 * the words that describe the machine and how a plain boot looks (the
 * hardware switches, verbose, nosplash, nousb, crashkernel=, ...), none
 * that pick a one-time run (ktest, bench, stress=, soak=, test<name>,
 * init, ...), so either way in is a plain boot; the test word
 * crashtest=<name> becomes test<name> (tools/kdump-test.sh). Cut to fit
 * buf. */
void kexec_next_cmdline(const char *from, char *buf, size_t size);

/* The panic path (debug/panic.c), all without a lock or an allocation.
 * begin: note where the panic's lines start in the log and decide: true if
 * the stored kernel will be started (armed, intact, and this is not a
 * crash loop), so the panic draws nothing; false leaves the panic screen
 * to be drawn, and kexec_panic_why_not says why there is no jump (NULL:
 * there never was a stored kernel). message: the panic's one line, for
 * the next boot's banner (the first call wins). jump: only after begin
 * said true: the crash record, bus mastering off, the other CPUs sent
 * INIT, the screen filled with the splash background, the jump. */
bool           kexec_panic_begin(void);
void           kexec_panic_message(const char *msg);
const char    *kexec_panic_why_not(void);
_Noreturn void kexec_panic_jump(void);
/* Is a panic now a crash loop? This boot started after a panic
 * (after_panic) and has run for uptime_ns. */
bool kexec_crash_loop(bool after_panic, uint64_t uptime_ns);

/* The name of this boot's log file ("boot-0042": 1..31 of [A-Za-z0-9_-]),
 * kept for the next boot to name its copy of the log after
 * (<name>-crash.txt) if this one panics. ERR_INVALID_ARGS for anything
 * else. */
status_t kexec_set_log_name(const char *name, size_t len);

/* Replace the stored kernel: the kernel ELF and the bootfs image (both
 * whole, read-only), and the command line ("" or NULL:
 * kexec_next_cmdline's). ERR_NOT_SUPPORTED: no region; ERR_INVALID_ARGS:
 * not a kernel ELF, not a bootfs image, a bad command line;
 * ERR_NO_RESOURCES: it doesn't fit; ERR_NO_MEMORY. A refused image
 * changes nothing (the checks come before the region is written): the old
 * one stays armed. While the new one is written there is none (a panic
 * then halts on its screen). Sleeps: thread context. */
status_t kexec_load_image(struct vmo *kernel, struct vmo *bootfs, const char *cmdline);
/* Start the stored kernel: interrupts off, the other CPUs halted, the
 * image verified, the screen the splash background, bus mastering off,
 * the jump. ERR_NOT_SUPPORTED without a region, ERR_BAD_STATE if nothing
 * is stored; a damaged image resets the machine through the firmware
 * instead. */
status_t kexec_reboot(void);

/* Tests. Recompute the stored kernel's checksum and compare (true: it
 * matches; false also with nothing stored). Flip a byte of the stored
 * kernel (the crash test kexecbad: the next panic must refuse it). */
bool     kexec_verify(void);
status_t kexec_test_corrupt(void);

/* Memory maps (kernel/kexec/memmap.c). Give [base, base + len) the type
 * `type` in map[0..*n): every entry it overlaps is split (at most two more
 * entries each); parts of the range no entry covers stay uncovered. The
 * map stays sorted if it was. ERR_NO_RESOURCES if it would grow past cap,
 * ERR_INVALID_ARGS if the range wraps; either way the map is unchanged.
 * And merge neighbours (touching, of one type) into one entry. */
status_t kexec_memmap_overlay(struct boot_mem_region *map, size_t *n, size_t cap,
                              uint64_t base, uint64_t len, enum boot_mem_type type);
void     kexec_memmap_merge(struct boot_mem_region *map, size_t *n);

/* ---- after a kexec (kernel/kexec/crashlog.c) ------------------------------------ */

/* kmain_stage2, with the heap up and before anything can panic into a
 * stored kernel: read the previous kernel's crash record (bi->kexec_record)
 * and, if it panicked, copy its log ring into a VMO for init
 * (SR_CRASHLOG); then free the CRASH_LOG pages. Logs what it found. Every
 * byte of it is untrusted. */
void crashlog_init(const struct boot_info *bi);
/* This boot was started by a kernel that panicked. */
bool crashlog_after_panic(void);
/* Panics in a row before this boot (0 if it wasn't started by one). */
uint32_t crashlog_panics(void);
/* The log VMO, handed over (the caller owns the reference), or NULL if
 * there is none or it was taken already. */
struct vmo *crashlog_take_vmo(void);
