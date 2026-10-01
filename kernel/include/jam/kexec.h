/* kexec: starting another Jam OS kernel without the firmware
 * (kernel/kexec/; the plan is docs/M8.5-PLAN.md).
 *
 * At boot the kernel reserves a physically contiguous region of RAM
 * (crashkernel=<MiB>, default 128, below 4 GiB) and unmaps it: the HHDM
 * skips it (BOOT_MEM_FOREIGN) and nothing else maps it, so no wild write
 * can reach it. Into it goes a ready-to-run kernel: its segments, its
 * bootfs, the page tables it starts on and the handoff it reads
 * (<jam/kexec_handoff.h>), checksummed once written. Two kinds:
 *   - the crash kernel, loaded at boot from the boot modules (Limine loads
 *     jamos.elf a second time as a module: the running kernel's own image
 *     is not pristine). A panic verifies the checksum, turns bus mastering
 *     off and jumps into it on the panicking CPU (kexec_panic_jump); it
 *     boots with only the region as its memory, saves the crashed kernel's
 *     log ring to the stick and halts or reboots (crashlog.c);
 *   - a reboot image (kexec_load_image, the kexec_load system call), which
 *     replaces the crash kernel: kexec_reboot jumps into it with all of
 *     memory.
 *
 * State (one word, read by the panic path without a lock): OFF (no
 * region), EMPTY, LOADING, ARMED (a crash kernel), IMAGE (a reboot image),
 * JUMPING. Every change of it and every write to the region holds the
 * kexec mutex; the panic path takes nothing and jumps only from ARMED. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <jam/boot.h>
#include <jam/status.h>

#define KEXEC_DEFAULT_MIB 128
#define KEXEC_KERNEL_MODULE "jamos.elf"   /* the pristine kernel: a module path's suffix */

struct vmo;

/* ---- the running kernel ------------------------------------------------------- */

/* kmain, before the memory managers: take the region out of bi's memory
 * map (it becomes BOOT_MEM_FOREIGN) unless the command line says
 * crashkernel=0 or this is a crash kernel ("crash"). Logs what it did. */
void kexec_reserve(struct boot_info *bi);
/* kmain_stage2, with the heap up and before the other CPUs start: load
 * the crash kernel from the boot modules and arm it. Logs what it did;
 * a failure leaves the region EMPTY (a panic then halts as it always
 * did). */
void kexec_crash_load(void);
/* The region: base and size in bytes; false if there is none. */
bool kexec_region(uint64_t *base, uint64_t *size);
/* A crash kernel is armed (the panic path would jump). */
bool kexec_crash_armed(void);

/* The panic path (debug/panic.c). begin: the panic's first byte is now
 * (the log's head); tail: the copy of the log tail starts now. jump: if a
 * crash kernel is armed, verify it and start it; returns only if it can't
 * (after a line on the screen saying why, or none if there was never a
 * crash kernel). No lock, no allocation from the decision on. */
void kexec_panic_begin(void);
void kexec_panic_tail(void);
void kexec_panic_jump(void);

/* The name of this boot's log file ("boot-0042": 1..31 of [A-Za-z0-9_-]),
 * kept for a crash kernel to name its copy of the log after
 * (<name>-crash.txt). ERR_INVALID_ARGS for anything else. */
status_t kexec_set_log_name(const char *name, size_t len);

/* Replace the crash kernel by a reboot image: the kernel ELF and the
 * bootfs image (both whole, read-only), and the command line ("" or NULL:
 * this kernel's). ERR_NOT_SUPPORTED: no region; ERR_INVALID_ARGS: not a
 * kernel ELF, not a bootfs image, a bad command line; ERR_NO_RESOURCES:
 * it doesn't fit; ERR_NO_MEMORY. On a failure there is no image and no
 * crash kernel either (the region is EMPTY). Sleeps: thread context. */
status_t kexec_load_image(struct vmo *kernel, struct vmo *bootfs, const char *cmdline);
/* Start the loaded reboot image: interrupts off, the other CPUs halted,
 * the image verified, bus mastering off, the jump. Returns ERR_BAD_STATE
 * if no image is loaded; a damaged image resets the machine through the
 * firmware instead. */
status_t kexec_reboot(void);

/* Tests. Recompute the loaded image's checksum and compare (true: it
 * matches; false also with nothing loaded). Flip a byte of the loaded
 * image (the crash test kexecbad: the next panic must refuse it). */
bool     kexec_verify(void);
status_t kexec_test_corrupt(void);

/* ---- a crash kernel ------------------------------------------------------------ */

/* This kernel was started by a panicking one ("crash" on its command line). */
bool kexec_is_crash_kernel(void);
/* kmain_stage2, with the heap up: find the crashed kernel's record and
 * ring (crashlog=<phys>, BOOT_MEM_CRASH_LOG), check them and copy the log
 * into a VMO for init (SR_CRASHLOG). Logs what it found. */
void crashlog_init(const struct boot_info *bi);
/* That VMO (a new reference), or NULL if there is no log. */
struct vmo *crashlog_vmo(void);
/* After init has ended: the crashed kernel's panic lines, the RESULTS
 * box, then halt, or after crash_reboot=<s> seconds a firmware reboot. */
_Noreturn void crashlog_finish(void);
