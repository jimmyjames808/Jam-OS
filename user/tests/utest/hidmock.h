/* utest's mock usb-bus and mock console for the hid driver (hidmock.c),
 * and the tests that drive it (hid.c). */
#pragma once

#include <os.h>

extern const char *hcur;   /* the hid test running (hid.c sets it) */

/* The mock's port keys: its ends of DR_USB and DR_INPUT, the driver's process. */
#define KEY_USB   1
#define KEY_INPUT 2
#define KEY_PROC  3

/* An interface of a mock device. */
struct mock_if {
    uint8_t        num, cls, subclass, protocol;   /* interface number, class triple */
    uint8_t        ep;         /* interrupt IN address */
    uint8_t        maxp;       /* its max packet */
    const uint8_t *rdesc;      /* the report descriptor */
    uint16_t       rlen;       /* its length */
};

/* A mock device: its interfaces. */
struct mock_dev {
    uint16_t              vendor, product;   /* USB ids */
    const struct mock_if *ifs;               /* interfaces */
    unsigned              nifs;              /* how many */
    bool                  no_report_protocol;   /* SET_PROTOCOL(report) stalls */
};

/* The recorded devices: a QEMU-style boot keyboard, a boot mouse with a
 * wheel, and a composite keyboard like the PC's (boot keyboard, report
 * protocol with consumer and system control, a mass storage interface).
 * Mice for the report protocol: a boot mouse without a wheel (it stays in
 * the boot protocol); a gaming mouse (16-bit X/Y, wheel, AC Pan, no report
 * ids), alone and as a device that refuses SET_PROTOCOL(report); a
 * receiver whose boot-mouse interface has a keyboard (id 1), the mouse
 * (id 2, 12-bit X/Y) and consumer control (id 3). */
extern const struct mock_dev dev_kbd, dev_mouse, dev_combo;
extern const struct mock_dev dev_plain_mouse, dev_gaming, dev_gaming_stubborn, dev_receiver;

/* A control request hid made (the mock's log). */
struct ctl {
    uint8_t  type, request;          /* bmRequestType, bRequest */
    uint16_t value, index, length;   /* wValue, wIndex, wLength */
    uint8_t  data0;                  /* the first data byte (an OUT's) */
};

#define EV_KEY   1
#define EV_MOUSE 2

/* An input event hid sent (the mock console's log). */
struct ev {
    uint8_t  kind;                   /* EV_KEY or EV_MOUSE */
    uint8_t  state, mods, buttons;   /* key: INPUT_KEY_*, modifiers; mouse: buttons */
    uint16_t usage;                  /* key usage */
    int16_t  dx, dy;                 /* mouse movement */
    int8_t   wheel;                  /* mouse wheel */
    uint32_t cp;                     /* key codepoint */
    uint64_t t;                      /* when it arrived (uptime ns) */
};

#define MAX_CTL 32
#define MAX_EV  512

/* One hid under test: the mock usb-bus and console it talks to. */
struct mock {
    const struct mock_dev *dev;       /* the device it is shown */
    const struct mock_if  *itf;       /* its interface under test */
    uint8_t   config[256];            /* the device's configuration descriptor */
    uint16_t  config_len;             /* its length */
    handle_t  job, proc, port;        /* hid's job and process; the port we wait on */
    handle_t  usb, input;       /* our ends: we serve usb and input */
    handle_t  reports;          /* usb-bus's end of the reports channel */
    bool      usb_closed_by_peer, input_closed_by_peer;   /* hid closed its end */
    unsigned  report_desc_reads;      /* GET_DESCRIPTOR(report) requests */
    unsigned  refused;                /* requests the mock refused */
    unsigned  opens;                  /* open_interrupt_in calls */
    struct ctl ctl[MAX_CTL];          /* control requests, in order */
    unsigned  nctl;                   /* how many */
    struct ev ev[MAX_EV];             /* input events, in order */
    unsigned  nev;                    /* how many */
};

/* Start drv/hid on interface `ifn` of dev, with a console unless
 * !with_console, and wait until it serves reports (unless !wait_ready). */
bool mock_start(struct mock *m, const struct mock_dev *dev, unsigned ifn, bool with_console,
                bool wait_ready);
/* mock_start with a console, waiting until it serves reports, and with the
 * word arg after the driver's name ("hidboot"; NULL: none). */
bool mock_start_arg(struct mock *m, const struct mock_dev *dev, unsigned ifn, const char *arg);
/* Serve the driver until done(m, arg) holds (true), the deadline passes or
 * the driver is dead without it (false). */
bool mock_pump(struct mock *m, uint64_t deadline, bool (*done)(struct mock *, unsigned),
               unsigned arg);
/* Serve the driver for ns. */
void mock_pump_for(struct mock *m, uint64_t ns);
/* mock_pump's condition: at least n events arrived. */
bool mock_have_events(struct mock *m, unsigned n);
/* Close what we still hold of the unplugged device: DR_USB and/or reports. */
void mock_unplug(struct mock *m, bool usb, bool reports);
/* The driver must end by itself with exit code `code`; every channel end
 * we still hold sees it gone; then its job is empty. */
bool mock_finish(struct mock *m, int code);
