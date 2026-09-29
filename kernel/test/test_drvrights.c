/* M7 Track D: a driver can't pass its hardware handles on (review of M6
 * phase 2, finding 2). devmgr hands them over with channel_write_rights
 * (syscall 121) minus RIGHT_DUPLICATE and RIGHT_TRANSFER; a physical VMO
 * made from such a BAR inherits that; kdevmgr gives kernel-process drivers
 * the same rights.
 *
 *   chan_write_rights            the system call's rules
 *   phys_vmo_keeps_no_transfer   vmo_create_physical inherits the limits
 *   kdev_driver_handles_stay     edu's handles (kernel-process driver) */
#include <jam/channel.h>
#include <jam/driver.h>
#include <jam/handle.h>
#include <jam/kdevmgr.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/pci.h>
#include <jam/process.h>
#include <jam/resource.h>
#include <jam/resource_impl.h>
#include <jam/sys.h>
#include <jam/time.h>
#include <jam/userboot.h>
#include <idl/edu.h>

#define S        1000000000ull
#define PASS_ON  (RIGHT_DUPLICATE | RIGHT_TRANSFER)
#define KEEP     (RIGHT_WAIT | RIGHT_INSPECT)

static rights_t rights_of(struct handle_table *t, handle_t h)
{
    struct kobject *o;
    rights_t r = 0;
    if (handle_get(t, h, OBJ_NONE, 0, &o, &r) == OK)
        kobject_unref(o);
    return r;
}

KTEST(chan_write_rights)
{
    struct handle_table t;
    handle_table_init(&t);
    handle_t a, b, e, e2, got, other_a, other_b;
    KT_EQ(sys_channel_create(&t, &a, &b), OK);
    KT_EQ(sys_channel_create(&t, &other_a, &other_b), OK);
    KT_EQ(sys_event_create(&t, &e), OK);
    rights_t full = rights_of(&t, e);
    KT_ASSERT(full & RIGHT_TRANSFER);
    uint8_t msg[4] = { 1, 2, 3, 4 }, in[4];
    uint32_t nb = 0, nh = 0;

    /* More rights than it has: refused, the handle stays as it was. */
    rights_t r = full | RIGHT_MANAGE;
    KT_EQ(sys_channel_write_rights(&t, a, msg, 4, &e, &r, 1), ERR_INVALID_ARGS);
    KT_EQ(rights_of(&t, e), full);

    /* Fewer: it arrives with exactly those, and can't go any further. */
    r = KEEP | RIGHT_SIGNAL;
    KT_EQ(sys_channel_write_rights(&t, a, msg, 4, &e, &r, 1), OK);
    KT_EQ(rights_of(&t, e), 0);   /* gone from the sender */
    KT_EQ(sys_channel_read(&t, b, in, 4, &nb, &got, 1, &nh), OK);
    KT_EQ(nh, 1);
    KT_EQ(rights_of(&t, got), KEEP | RIGHT_SIGNAL);
    KT_EQ(handle_duplicate(&t, got, RIGHT_SAME, &e2), ERR_ACCESS_DENIED);
    KT_EQ(sys_channel_write(&t, other_a, msg, 4, &got, 1), ERR_ACCESS_DENIED);
    r = RIGHT_SAME;
    KT_EQ(sys_channel_write_rights(&t, other_a, msg, 4, &got, &r, 1), ERR_ACCESS_DENIED);
    KT_EQ(rights_of(&t, got), KEEP | RIGHT_SIGNAL);   /* still ours */

    /* RIGHT_SAME passes it unchanged; a failed write (peer gone) puts it
     * back with its rights. */
    KT_EQ(sys_event_create(&t, &e), OK);
    r = RIGHT_SAME;
    KT_EQ(sys_channel_write_rights(&t, a, msg, 4, &e, &r, 1), OK);
    KT_EQ(sys_channel_read(&t, b, in, 4, &nb, &e2, 1, &nh), OK);
    KT_EQ(rights_of(&t, e2), full);
    KT_EQ(handle_close(&t, b), OK);
    r = KEEP;
    KT_EQ(sys_channel_write_rights(&t, a, msg, 4, &e2, &r, 1), ERR_PEER_CLOSED);
    KT_EQ(rights_of(&t, e2), full);
    KT_EQ(sys_channel_write_rights(&t, a, msg, 4, &e2, NULL, 1), ERR_INVALID_ARGS);
    handle_table_destroy(&t);
}

KTEST(phys_vmo_keeps_no_transfer)
{
    struct pci_dev *d = pci_find(0x1234, 0x11e8, 0);
    if (!d) {
        kprintf("ktest %s: no edu, skipped\n", ktest_current);
        return;
    }
    struct handle_table t;
    handle_table_init(&t);
    struct kobject *root = resource_root(), *pci, *dev, *bar;
    KT_EQ(resource_create(root, RES_PCI, 0, 0, &pci), OK);
    KT_EQ(resource_pci_device(pci, d->index, &dev), OK);
    KT_EQ(resource_pci_bar(dev, 0, &bar), OK);
    struct khandle kh = khandle_from_new(bar, KEEP | RIGHT_MAP | RIGHT_TRANSFER);
    handle_t h, v1, v2;
    KT_EQ(handle_insert(&t, &kh, &h), OK);
    KT_EQ(sys_vmo_create_physical(&t, h, 0, PAGE_SIZE, VMO_CACHE_UC, &v1), OK);
    KT_EQ(rights_of(&t, v1) & PASS_ON, RIGHT_TRANSFER);   /* what the BAR handle had */
    handle_t h2;
    KT_EQ(handle_replace(&t, h, KEEP | RIGHT_MAP, &h2), OK);   /* a driver's BAR */
    KT_EQ(sys_vmo_create_physical(&t, h2, 0, PAGE_SIZE, VMO_CACHE_UC, &v2), OK);
    KT_EQ(rights_of(&t, v2) & PASS_ON, 0);
    KT_ASSERT(rights_of(&t, v2) & RIGHT_MAP);   /* still the driver's to map */
    handle_table_destroy(&t);
    kobject_unref(dev);
    kobject_unref(pci);
    kobject_unref(root);
}

KTEST(kdev_driver_handles_stay)
{
    struct pci_dev *d = pci_find(0x1234, 0x11e8, 0);
    if (!d) {
        kprintf("ktest %s: no edu, skipped\n", ktest_current);
        return;
    }
    struct job *root;
    KT_EQ(userboot_root_job(&root), OK);
    struct kdev_binding b;
    KT_EQ(kdev_bind(d, "edu", root, &b), OK);
    /* Once it answers, it has set up (and holds all it was given). */
    struct edu_factorial_req q = { 0, EDU_FACTORIAL, 5 };
    struct edu_factorial_rep rep;
    uint32_t n = 0;
    KT_EQ(channel_call((struct channel *)b.client.obj, &q, sizeof(q), NULL, 0, &rep, sizeof(rep),
                       &n, NULL, 0, NULL, uptime_ns() + 10 * S),
          OK);
    struct handle_table *t = process_handles(b.proc);
    static const struct { enum obj_type type; const char *what; } kinds[] = {
        { OBJ_DMA_CAP, "dma_cap" }, { OBJ_INTERRUPT, "interrupt" }, { OBJ_RESOURCE, "resource" },
    };
    for (unsigned k = 0; k < 3; k++) {
        rights_t r[8];
        uint32_t cnt = handle_table_rights(t, kinds[k].type, r, 8);
        kprintf("ktest %s: %u %s handle(s)\n", ktest_current, cnt, kinds[k].what);
        KT_ASSERT(cnt >= 1 && cnt <= 8);
        for (uint32_t i = 0; i < cnt; i++)
            KT_EQ(r[i] & PASS_ON, 0);
    }
    KT_ASSERT(kdev_unbind(&b, 10 * S));
    job_unref(root);
}
