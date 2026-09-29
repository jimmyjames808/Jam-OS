/* System calls for interrupt objects (M6, Track B): interrupt_create_msi
 * and interrupt_ack, plus their handle-level layer (sys.h). The rules
 * every sysc_* follows are in sysc.h. */
#include <jam/interrupt.h>
#include <jam/resource.h>
#include <jam/sys.h>
#include <jam/syscall_impl.h>
#include "sysc.h"

#define INTERRUPT_RIGHTS (RIGHTS_BASIC | RIGHTS_IO)

status_t sys_interrupt_create_msi(struct handle_table *t, handle_t dev, uint32_t index,
                                  uint32_t flags, handle_t *out)
{
    if (flags & ~IRQ_MSIX)
        return ERR_INVALID_ARGS;
    struct kobject *res;
    status_t st = handle_get(t, dev, OBJ_RESOURCE, RIGHT_MANAGE, &res, NULL);
    if (st != OK)
        return st;
    struct pci_dev *d = resource_pci_dev(res);   /* NULL unless RES_PCI_DEV */
    struct kobject *irq = NULL;
    st = d ? interrupt_create_msi(d, index, flags, &irq) : ERR_WRONG_TYPE;
    kobject_unref(res);
    if (st != OK)
        return st;
    struct khandle kh = khandle_from_new(irq, INTERRUPT_RIGHTS);
    st = handle_insert(t, &kh, out);
    if (st != OK)
        khandle_release(&kh);   /* disables it at the device and frees the vector */
    return st;
}

status_t sys_interrupt_ack(struct handle_table *t, handle_t h)
{
    struct kobject *obj;
    status_t st = handle_get(t, h, OBJ_INTERRUPT, RIGHT_WRITE, &obj, NULL);
    if (st != OK)
        return st;
    st = interrupt_ack(obj);
    kobject_unref(obj);
    return st;
}

int64_t sysc_interrupt_create_msi(handle_t dev, uint32_t index, uint32_t flags, uint64_t out)
{
    SYSC_TABLE(t);
    handle_t h;
    status_t st = sys_interrupt_create_msi(t, dev, index, flags, &h);
    return st == OK ? sysc_put_handle(t, out, h) : st;
}

int64_t sysc_interrupt_ack(handle_t irq)
{
    SYSC_TABLE(t);
    return sys_interrupt_ack(t, irq);
}
