/* What is left of devmgr's header: the protocol is abi/idl/devmgr.idl
 * (<idl/devmgr.h>: devmgr, which any holder of a channel to devmgr
 * speaks, and devmgrctl, the control channel's; the crash-test driver's is
 * abi/idl/crasher.idl), and its trust and reconnect rules are that file's
 * opening paragraphs. Here: what is not the protocol. */
#pragma once

#include <os.h>

/* devmgr's startup role: a duplicate of /svc/notify's shared channel, for
 * the desktop's notices of drivers that crash or
 * are given up on (<notice.h>). SR_USER + 0 was the ESP channel's until
 * the storage split. */
#define DEVMGR_SR_NOTIFY      (SR_USER + 1)

/* What devmgr gives each driver (and devmgrctl.driver_view and driver_bar
 * hand out, with RIGHT_TRANSFER besides). None of a
 * driver's hardware handles can be duplicated or passed on (no
 * RIGHT_DUPLICATE, no RIGHT_TRANSFER; a physical VMO made from such a BAR
 * inherits that): a driver can't smuggle its function, registers,
 * interrupt or dma_cap out through DR_SERVE to outlive it. devmgr makes
 * them with RIGHT_TRANSFER and hands
 * them over with channel_write_rights. */
#define DEVMGR_DRV_KEEP       (RIGHT_WAIT | RIGHT_INSPECT)
#define DEVMGR_DRV_DEV_RIGHTS (DEVMGR_DRV_KEEP | RIGHT_READ | RIGHT_WRITE)
#define DEVMGR_DRV_BAR_RIGHTS (DEVMGR_DRV_KEEP | RIGHT_MAP)
#define DEVMGR_DRV_IRQ_RIGHTS (DEVMGR_DRV_KEEP | RIGHT_READ | RIGHT_WRITE)
#define DEVMGR_DRV_DMA_RIGHTS DEVMGR_DRV_KEEP
/* A class driver's channels (DR_USB, DR_INPUT). */
#define DEVMGR_DRV_CHAN_RIGHTS (DEVMGR_DRV_KEEP | RIGHT_READ | RIGHT_WRITE)
