/* nvme: the NVM Express driver (drv/nvme), started by devmgr for every
 * PCI function of class 01 08 02. READ ONLY: it has no code that writes a
 * block or changes the drive (cmd.c, tools/checknvme.py; the plan is
 * docs/M12.7-PLAN.md).
 *
 * Handles (roles from <jam/driver.h>):
 *   DR_BAR(0)   the registers and doorbells (the MSI-X table is the
 *               kernel's: never mapped)
 *   DR_DMA      its dma_cap: bus mastering goes on once the controller is
 *               disabled (ctrl.c)
 *   DR_IRQ(0)   MSI-X vector 0: the admin and the I/O completion queues
 *               (without one: polled every millisecond)
 *   DR_PCIDEV   its function
 *   DR_SERVE    the channel it serves `storage` on (abi/idl/storage.idl),
 *               with svc.connect: a `storage` channel of the caller's own
 *               (devmgr asks one for volumes); closed: devmgr is stopping
 *               us
 * Arguments: `pci=<bus>:<dev>.<fn>` (devmgr's: the name in the log), and
 * the test words `nvmetest=selftest[,loop][,reset][,abort]` (selftest.c).
 *
 * Start (ctrl.c): the controller disabled before bus mastering, the admin
 * queue, enabled, identify (the controller; the first usable namespace),
 * one I/O queue pair on vector 0. Then the partition table (serve.c),
 * its health log once (the drive's own count of write commands: the
 * witness that nothing was written), a RESULTS line, and the loop. One
 * thread, one port: the interrupt, DR_SERVE and every channel it serves
 * (serve.c, block.c) arrive there; the loop also wakes at the nearest command's
 * deadline (and every second while commands are out, so a lost interrupt
 * costs a second, never an answer).
 *
 * Ending: DR_SERVE closed: reads failed (answered ERR_PEER_CLOSED), the
 * I/O queues deleted, the
 * normal shutdown notification (the drive saves its state, its unsafe
 * shutdown count doesn't grow), bus mastering off, every channel closed
 * and everything unpinned,
 * exit 0. Exit 0 also when the controller has nothing to serve (no usable
 * namespace, not the NVM command set: said in the RESULTS box). Exit 1:
 * missing handles, or a command outside the read-only list (cmd.c); 3:
 * the controller couldn't be brought up, went away, or needed more than
 * RESETS_MAX resets in RESET_WINDOW (devmgr restarts it with backoff). */
#include <idl/common.h>
#include "nvme.h"

#define POLL_BUSY NS_PER_S          /* commands out: look at least this often */
#define POLL_NOIRQ NS_PER_MS        /* no interrupt object: polled */

static struct nvme ctrl;
static char name[32];

/* The name in the log: "nvme 02:00.0" from devmgr's pci= word. */
static void set_name(struct nvme *c, const struct driver_start *s)
{
    drv_snprintf(name, sizeof(name), "nvme");
    for (uint32_t i = 0; i < s->nargs && i < DRV_MAX_ARGS; i++) {
        const char *a = s->args[i];
        if (a && a[0] == 'p' && a[1] == 'c' && a[2] == 'i' && a[3] == '=')
            drv_snprintf(name, sizeof(name), "nvme %s", a + 4);
    }
    c->name = name;
}

/* The handles and the port. 0, or the exit code. */
static int take_handles(struct nvme *c, const struct driver_start *s, handle_t *bar)
{
    *bar = drv_handle(s, DR_BAR(0));
    c->dev = drv_handle(s, DR_PCIDEV);
    c->dma = drv_handle(s, DR_DMA);
    c->irq = drv_handle(s, DR_IRQ(0));
    c->serve = drv_handle(s, DR_SERVE);
    if (*bar == HANDLE_INVALID || c->dma == HANDLE_INVALID || c->serve == HANDLE_INVALID) {
        drv_log("%s: missing handles (BAR 0 %#x, DMA %#x, serve %#x): nothing touched", c->name,
                *bar, c->dma, c->serve);
        return 1;
    }
    status_t st = drv_port_create(&c->port);
    if (st == OK && c->irq != HANDLE_INVALID)
        st = drv_port_bind(c->port, c->irq, KEY_IRQ, SIG_INTERRUPT, PORT_BIND_PERSISTENT);
    if (st != OK) {
        drv_log("%s: no port (%s): nothing touched", c->name, status_str(st));
        return 1;
    }
    return 0;
}

static void report(struct nvme *c)
{
    struct nvme_health h;
    status_t st = ctrl_health(c, &h);
    if (st == OK)
        drv_log("%s: health: host write commands %lu, host read commands %lu, data units "
                "written %lu, read %lu, power cycles %lu, unsafe shutdowns %lu, media errors %lu, "
                "%u%% used, critical warning %#x", c->name, (unsigned long)h.host_writes,
                (unsigned long)h.host_reads, (unsigned long)h.units_written,
                (unsigned long)h.units_read, (unsigned long)h.power_cycles,
                (unsigned long)h.unsafe_shutdowns, (unsigned long)h.media_errors,
                h.percent_used, h.critical);
    else
        drv_log("%s: health log: %s", c->name, status_str(st));
    drv_log("%s: %s (serial %s, firmware %s), NVMe %u.%u, namespace %u: %lu blocks of %u bytes",
            c->name, c->model, c->serial, c->firmware, c->vs >> 16, (c->vs >> 8) & 0xff,
            c->ns.nsid, (unsigned long)c->ns.blocks, c->ns.block_size);
    /* The RESULTS box cuts long lines: the model, the size, the table. */
    char table[80];
    serve_describe(table, sizeof(table));
    unsigned shift = 20 - (unsigned)__builtin_ctz(c->ns.block_size);
    drv_report("%s: %s, %lu MiB, %s, read-only", c->name, c->model,
               (unsigned long)(c->ns.blocks >> shift), table);
    drv_log("%s: queues of %u entries, %u bytes a command at most, %s", c->name, c->io.depth,
            c->max_cmd, c->irq != HANDLE_INVALID ? "msi-x vector 0" : "polled");
}

/* Until DR_SERVE closes. OK, or why the controller can't go on. */
static status_t run(struct nvme *c)
{
    status_t st = serve_watch(c);
    while (st == OK) {
        if ((st = ctrl_poll(c)) != OK)
            break;
        if (!serve_work(c))
            break;   /* DR_SERVE closed */
        blk_due(c);
        uint64_t now = drv_clock_ns(), d = queue_next_deadline(c);
        d = d < blk_next_due() ? d : blk_next_due();
        if (c->admin.busy || c->io.busy || c->wait_head)
            d = d < now + POLL_BUSY ? d : now + POLL_BUSY;
        if (c->irq == HANDLE_INVALID)
            d = d < now + POLL_NOIRQ ? d : now + POLL_NOIRQ;
        struct port_packet p;
        st = drv_port_wait(c->port, serve_ready() ? 0 : d, &p);
        if (st == ERR_TIMED_OUT) {
            c->polls++;
            st = OK;
        } else if (st == OK && p.key == KEY_IRQ) {
            c->irqs += p.signal.count;
            (void)drv_interrupt_ack(c->irq);   /* before the queues are read: no fire lost */
        } else if (st == OK) {
            serve_packet(p.key);
        }
    }
    return st;
}

int driver_main(const struct driver_start *s)
{
    struct nvme *c = &ctrl;
    *c = (struct nvme){ .port = HANDLE_INVALID, .data_vmo = HANDLE_INVALID,
                        .prp_vmo = HANDLE_INVALID };
    set_name(c, s);
    c->test = selftest_words(s);
    handle_t bar;
    int r = take_handles(c, s, &bar);
    if (r)
        return r;
    status_t st = ctrl_start(c, bar);
    if (st == ERR_NOT_SUPPORTED || st == ERR_NOT_FOUND) {
        drv_report("%s: %s: not served", c->name, st == ERR_NOT_FOUND ? "no usable namespace"
                                                                        : "not a controller "
                                                                          "this driver can use");
        ctrl_stop(c);
        return 0;
    }
    if (st == OK)
        st = serve_start(c);
    if (st == OK) {
        report(c);
        if ((c->test & NVME_TEST_SELFTEST) && !selftest_start(c))
            drv_report("%s: selftest: FAILED to start", c->name);
        st = run(c);
    }
    c->stopping = true;
    ctrl_stop(c);
    serve_stop(c);
    selftest_stop(c);
    drv_log("%s: stopped (%s): %lu read(s), %lu failed, %lu completion(s) (%lu stale), %lu "
            "interrupt(s), %lu poll(s), %lu abort(s), %u reset(s); bus mastering %s", c->name,
            st == OK ? "devmgr closed our channel" : status_str(st),
            (unsigned long)c->reads, (unsigned long)c->read_fail, (unsigned long)c->completions,
            (unsigned long)c->stale, (unsigned long)c->irqs, (unsigned long)c->polls,
            (unsigned long)c->aborts, c->resets, c->bus_master ? "STILL ON" : "off");
    return st == OK ? 0 : 3;
}
