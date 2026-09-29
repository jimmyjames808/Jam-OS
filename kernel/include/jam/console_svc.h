/* M7 Track C: the kernel services the console and the shell use
 * (kernel/abi/sysc_console.c, kernel/core/dbgcmd.c, kernel/core/reboot.c).
 * The system calls are 110-117 in abi/syscalls.def; these are their
 * kernel halves, on objects, so ktests can drive them without a process.
 *
 *   OBJ_KLOG    a kernel log reader: SIG_READABLE while the log holds bytes
 *               past what it last read (raised by CPU 0's tick).
 *   OBJ_SERIAL  COM1 input (one at a time): SIG_READABLE while bytes wait.
 *   OBJ_SCREEN  ownership of the boot framebuffer: while a handle to it is
 *               open, fbcon draws nothing; the last handle closing gives the
 *               screen back (fbcon redraws). */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <jam/abi.h>
#include <jam/object.h>
#include <jam/status.h>

struct job;
struct vmo;

/* Readers. job (may be NULL) is charged one JOB_LIMIT_HANDLES unit. */
status_t klog_reader_create(struct job *job, struct kobject **out);
/* Up to cap bytes from pos into buf (a kernel buffer); *first as in
 * klog_read_at. Updates the reader's SIG_READABLE. */
size_t   klog_reader_read(struct kobject *reader, uint64_t pos, char *buf, size_t cap,
                          uint64_t *first);

/* COM1 input. ERR_NOT_FOUND: no UART; ERR_BAD_STATE: someone reads it. */
status_t serial_in_create(struct job *job, struct kobject **out);
size_t   serial_in_read(struct kobject *in, char *buf, size_t cap);

/* The screen: *vmo (WC physical VMO over the framebuffer, charged to job)
 * and *owner (OBJ_SCREEN). The screen is taken from the moment this returns
 * OK until the owner's last handle closes (an owner that never gets a
 * handle: screen_owner_drop). */
status_t screen_take(struct job *job, struct fb_info *info, struct vmo **vmo,
                     struct kobject **owner);
void     screen_owner_drop(struct kobject *owner);   /* unref, releasing if never handled */

/* debug_command: run cmd (ktest / bench / stress / devices / ps) in a kernel
 * thread and wait for it (cancellable: ERR_CANCELED leaves it running).
 * scope (may be NULL) is the job tree "ps" lists. */
int64_t  dbgcmd_run(const char *cmd, size_t len, struct job *scope);
bool     dbgcmd_busy(void);
/* The parse without running anything (tests): 0 if cmd is known. */
status_t dbgcmd_check(const char *cmd, size_t len);

/* Reset the machine: ACPI reset register, 0xCF9, 8042, triple fault. */
_Noreturn void machine_reboot(void);
/* Which reset methods this machine offers, for the log / RESULTS. */
void     reboot_describe(char *buf, size_t size);
