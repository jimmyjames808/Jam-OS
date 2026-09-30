/* usbtest: what its files share (main.c: the bus, the device list and
 * the USB checks). See main.c. */
#pragma once

#define CHECK_PROG "usbtest"
#define CHECK_CUR  cur
#include <check.h>
#include <os.h>

#define MAX_DEV 48

/* One device as usbbus.device reports it (the same fields). */
struct dev {
    uint32_t id, parent;             /* usb-bus's ids: its own, its hub's (0: a root port) */
    uint16_t vid, pid, bcd, mps0;    /* USB ids, bcdUSB, EP0 max packet */
    uint8_t speed, address, slot;    /* USB speed, address, xHCI slot */
    uint8_t root_port, port, level;  /* root port, port on its hub, tier (1: root port) */
    uint8_t tt_slot, tt_port;        /* its TT's hub slot and port, 0: none */
    uint8_t cls, sub, proto;         /* device class, subclass, protocol */
    uint8_t nconfigs, config;        /* configurations; the one set (0: unconfigured) */
    uint8_t nifs, hub_ports;         /* interfaces; a hub's ports (0: not a hub) */
    uint32_t route;                  /* route string */
    char path[25], name[41], serial[25];   /* "9.1", product, serial: NUL-terminated */
};

/* ---- main.c -------------------------------------------------------------------- */

extern const char *cur;          /* the test running */
extern unsigned skipped;         /* tests skipped so far (a test that skips says why) */
extern handle_t bus;             /* usb-bus's DR_SERVE (usbbus), from devmgr */
extern struct dev devs[MAX_DEV]; /* the device list, as load() last read it */
extern unsigned ndevs;           /* entries in devs[] */

uint64_t in(uint64_t ns);        /* the deadline `ns` from now */
uint64_t soon(void);             /* a usb-bus or usb call's deadline: 5 s from now */
status_t load(void);             /* read the device list into devs[] */
struct dev *by_serial(const char *s);
/* Run one test: counted as passed, failed or (if it said so) skipped. */
void run(const char *name, bool (*fn)(void));
