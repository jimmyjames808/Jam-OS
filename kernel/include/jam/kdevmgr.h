/* Drivers bound by the kernel itself (M6 phase 2): the kernel-process mode
 * of devmgr (kernel/drivers/kdevmgr.c). user/devmgr does the same job as a
 * process with system calls; this is the `drivers=kernel` boot word and
 * the ktests. Both give a driver exactly the same handles:
 *
 *   DR_PCIDEV  its function, RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE
 *              (filtered config access; no RIGHT_MANAGE: no bus mastering,
 *              no dma_cap, no interrupt objects; no RIGHT_SLICE / MAP)
 *   DR_BAR(n)  each memory BAR as a RES_MMIO, RIGHTS_BASIC | RIGHT_MAP
 *   DR_IRQ(0)  an interrupt object: MSI-X entry 0 if the function has
 *              MSI-X, else MSI (none if it has neither)
 *   DR_DMA     a dma_cap bound to the function; Bus Master Enable is on
 *              (MSI needs it too) until the cap's last handle goes
 *   DR_SERVE   a channel; the binder keeps the other end (the client end)
 *
 * The driver runs in a job of its own below the caller's job, with limits.
 * Killing it (process_kill, job_kill) closes its handles: the dma_cap turns
 * Bus Master Enable off and releases its pins, the interrupt object frees
 * its vector. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/handle.h>
#include <jam/process.h>
#include <jam/status.h>

struct pci_dev;

/* The match table (shared in spirit with user/devmgr's): vendor/device
 * (0xffff = any) and/or a class code (class << 16 | subclass << 8 |
 * prog_if; KDEV_ANY_CLASS = any). The first entry that matches wins; a
 * driver that isn't built in is skipped. */
#define KDEV_ANY_CLASS 0xffffffffu
struct kdev_match {
    uint16_t    vendor, device;
    uint32_t    class_code;
    const char *driver;
};
extern const struct kdev_match kdev_matches[];   /* ends with driver == NULL */
const char *kdev_match_driver(const struct pci_dev *d);

struct kdev_binding {
    struct pci_dev *dev;
    char            driver[32];
    struct job     *job;      /* the driver's own job (a reference) */
    struct process *proc;     /* the driver (a reference) */
    struct khandle  client;   /* the client end of its DR_SERVE channel */
    /* Extra references for checks (tests): the objects stay readable after
     * their handles are gone, they don't keep them alive. */
    struct kobject *irq;      /* NULL if the function has no MSI / MSI-X */
    struct kobject *dma_cap;
};

/* Bind `driver` (a built-in driver's name) to d: make the handles above,
 * start it as a kernel process in a new job below `parent`. */
status_t kdev_bind(struct pci_dev *d, const char *driver, struct job *parent,
                   struct kdev_binding *out);
/* Close the client end, wait up to timeout_ns for the driver to return,
 * kill its job if it doesn't, drop every reference. Returns true if it
 * exited 0 by itself and its job ended with nothing charged. */
bool kdev_unbind(struct kdev_binding *b, uint64_t timeout_ns);

/* The edu client check (factorial, DMA round trip, raise_irq) through the
 * generated <idl/edu.h> client, run in a kernel process of its own named
 * `name` (its drv_report line starts with it). OK if every check passed. */
status_t kdev_edu_check(struct kdev_binding *b, const char *name);

/* The `drivers=kernel` boot word: bind every device the table matches as a
 * kernel process, run the edu check on edu, unbind. true if all went well
 * (no matching device at all is fine: the PC has no edu). */
bool kdev_run_kernel_mode(void);
