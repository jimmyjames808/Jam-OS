/* M6 done test: the xhci-noop driver on the machine's xHCI controller
 * (QEMU: qemu-xhci 1b36:000d with MSI-X; the PC: Intel 8086:7a60), as a
 * kernel process and as a process (drv/xhci-noop from bootfs), with the
 * handles built in the kernel (kernel/drivers/xhci_launch.c). Each run
 * resets the controller; nothing in Jam OS uses USB yet and bootfs is in
 * memory already, so the boot stick on it doesn't matter. The driver
 * leaves the controller halted and reset; afterwards Bus Master Enable
 * and MSI / MSI-X must be off and the driver's job empty. The same runs
 * are the "xhcitest" boot entry (main.c), where the process run goes
 * through init and the M6 syscalls instead. */
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/pci.h>
#include <jam/xhci_launch.h>

static struct pci_dev *controller(void)
{
    struct pci_dev *d = xhci_find(0);
    if (!d)
        kprintf("%s: no xHCI controller (class 0c0330): skipped\n", ktest_current);
    return d;
}

KTEST(xhci_noop_kernel_process)
{
    struct pci_dev *d = controller();
    if (d)
        KT_ASSERT(xhci_launch(d, false));
}

KTEST(xhci_noop_user_process)
{
    struct pci_dev *d = controller();
    if (d)
        KT_ASSERT(xhci_launch(d, true));
}
