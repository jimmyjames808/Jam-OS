/* hid: what the driver's files share (hid.c: the device and the reports;
 * desc.c: walking its descriptors; keyboard.c: the keyboard layer;
 * mouse.c: the mouse layer; report.c and fixtures.c: the report
 * descriptor parser and its self-test, report.h). See hid.c for the
 * driver. */
#pragma once

#include <idl/input.h>   /* INPUT_KEY_*, INPUT_MOD_*, INPUT_READY_* */
#include <jam/driver.h>
#include <idl/input.h>
#include "report.h"

#define BUF_SIZE 1024   /* h->buf: descriptors, then reports */

/* Descriptor types (USB 2.0 table 9-5, HID 1.11 7.1) */
#define DESC_CONFIG    0x02
#define DESC_INTERFACE 0x04
#define DESC_ENDPOINT  0x05
#define DESC_HID       0x21
#define DESC_REPORT    0x22

/* Events kept while the compositor's queue is full (hid.c's outq):
 * enough for a burst of whole reports; past it a key event is dropped,
 * said in the log. */
#define OUTQ_MAX 256

enum hid_kind { HID_KEYBOARD = 1, HID_MOUSE = 2 };

enum hid_stop {
    STOP_NONE = 0,
    STOP_DEVICE_GONE,    /* DR_USB closed (or both): unplugged */
    STOP_REPORTS_LOST,   /* the reports channel closed, DR_USB still open */
    STOP_CONSOLE_GONE,   /* DR_INPUT closed: the compositor restarted */
};

struct kbd {
    uint8_t  mods;           /* modifier byte of the last report taken */
    uint8_t  leds;           /* INPUT_LED_*: the lights, as the compositor last said */
    uint8_t  nheld;          /* entries in held[] */
    uint8_t  held[6];        /* the usages down, in the order they went down */
    uint64_t rollover;       /* phantom (ErrorRollOver) reports ignored */
    uint64_t short_reports;  /* reports too short to be a boot report */
};

/* An event the compositor's full queue refused, kept to send later. */
struct out_ev {
    uint8_t  kind;           /* OUT_KEY, OUT_MOUSE, OUT_READY */
    uint8_t  a, b, c;        /* key: state, mods; mouse: buttons, wheel (as u8); ready: kind */
    uint16_t x, y;           /* key: usage; mouse: dx, dy (as u16); ready: vendor, product */
};
enum { OUT_KEY = 1, OUT_MOUSE, OUT_READY };

/* The driver's state: one interface. Only the driver's one thread uses it. */
struct hid {
    handle_t usb;            /* DR_USB: the `usb` channel of our interface */
    handle_t input;          /* DR_INPUT: the compositor's `input` channel, or HANDLE_INVALID */
    handle_t reports;        /* the report channel open_interrupt_in gave, or HANDLE_INVALID */
    handle_t port;           /* the loop's port, or HANDLE_INVALID */
    uint16_t vendor, product;   /* USB ids (usb.info) */
    uint8_t  iface, alt;     /* interface number and its active alternate setting */
    uint8_t  subclass, protocol;   /* 1 and 1 / 2: a boot keyboard / mouse */
    uint8_t  speed;          /* the device's USB speed */
    uint8_t  ep_in;          /* the interrupt IN endpoint's address, 0: none found */
    uint8_t  interval_ms;    /* its polling interval */
    uint16_t max_packet;     /* its max packet size */
    uint16_t report_desc_len;   /* from the HID descriptor, 0: none */
    uint16_t report_desc_read;  /* bytes of it read into buf (0: unreadable), until the
                                 * reports reuse buf */
    bool     force_boot;     /* the boot word hidboot (arg "hidboot"): every mouse in boot
                              * protocol */
    bool     report_mode;    /* a mouse in report protocol, decoded with `layout` */
    struct rd_mouse layout;  /* its report, from the report descriptor */
    enum hid_kind kind;      /* 0 until a boot keyboard or mouse is set up */
    enum hid_stop stop;      /* why the loop ends, STOP_NONE while it runs */
    uint64_t nreports;       /* reports taken */
    uint64_t events;         /* input events sent (or logged) */
    uint64_t input_errors;   /* input events that failed or were dropped (said in the log) */
    uint64_t waited;         /* events that waited in outq for room */
    struct out_ev outq[OUTQ_MAX];   /* events the full queue refused, oldest first */
    uint32_t outq_head, outq_n;
    bool     write_armed;    /* the port watches DR_INPUT for room */
    uint64_t led_errors;     /* LED SET_REPORTs that failed */
    uint64_t keys_down;      /* key DOWN events (for the RESULTS line at the end) */
    uint8_t  mouse_buttons;  /* the buttons of the last mouse report */
    uint64_t other_reports;  /* report protocol: reports of another report id, ignored */
    struct kbd kbd;          /* the keyboard layer (keyboard.c) */
    uint8_t *buf;            /* 1024 bytes: descriptors, reports */
};

/* hid.c: send one key event to the compositor (or to the log without
 * DR_INPUT); kept for later if its queue is full; sets h->stop when the
 * compositor is gone. */
void hid_key(struct hid *h, uint16_t usage, uint8_t state, uint8_t mods);
/* hid.c: SET_REPORT(output) with the LED byte. */
void hid_set_leds(struct hid *h, uint8_t leds);
/* hid.c: send one mouse event to the compositor (or, without DR_INPUT,
 * log the buttons when buttons_changed); one kept for later merges with
 * the last kept if their buttons are the same; sets h->stop when the
 * compositor is gone. */
void hid_mouse(struct hid *h, int16_t dx, int16_t dy, int8_t wheel, uint8_t buttons,
               bool buttons_changed);

/* desc.c: this interface's HID descriptor (h->report_desc_len) and
 * interrupt IN endpoint (h->ep_in, h->max_packet), from the configuration
 * descriptor in h->buf (n bytes). */
void hid_parse_config(struct hid *h, uint32_t n);
/* desc.c: the report descriptor in h->buf (n bytes) in one line into out
 * (cap bytes): "keyboard id 1, consumer control id 3, vendor (page 0xff00
 * usage 0x1) id 6". */
void hid_summarise_report(const struct hid *h, uint32_t n, char *out, uint32_t cap);

/* mouse.c: boot or report protocol for this mouse, from its report
 * descriptor (h->buf, h->report_desc_read bytes), which is logged in hex;
 * sets h->report_mode and h->layout. */
void mouse_choose(struct hid *h);
/* mouse.c: one report from the mouse, in the protocol chosen. */
void mouse_report(struct hid *h, const uint8_t *r, uint32_t n);

/* keyboard.c */
void     kbd_init(struct hid *h);
void     kbd_report(struct hid *h, const uint8_t *r, uint32_t n);
/* The compositor's input.leds: the lights set (SET_REPORT) if they changed. */
void     kbd_leds(struct hid *h, uint8_t leds);
