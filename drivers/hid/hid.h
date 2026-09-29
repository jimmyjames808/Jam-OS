/* hid: what the driver's two files share (hid.c: the device, the reports,
 * the mouse; keyboard.c: the keyboard layer). See hid.c for the driver. */
#pragma once

#include <jam/driver.h>

#define MS               1000000ull
#define REPEAT_DELAY_NS  (500 * MS)                /* held this long: REPEAT starts */
#define REPEAT_PERIOD_NS (1000000000ull / 30)      /* then 30 a second */

/* LED bits of the boot keyboard's output report (HID LED page 1..3). */
#define LED_NUM    0x01u
#define LED_CAPS   0x02u
#define LED_SCROLL 0x04u

enum hid_kind { HID_KEYBOARD = 1, HID_MOUSE = 2 };

enum hid_stop {
    STOP_NONE = 0,
    STOP_DEVICE_GONE,    /* DR_USB or the reports channel closed: unplugged */
    STOP_CONSOLE_GONE,   /* DR_INPUT closed: the console restarted */
};

struct kbd_held {
    uint8_t  raw;        /* the usage in the report */
    uint16_t usage;      /* the usage sent with DOWN (Num Lock may map the keypad) */
    uint32_t codepoint;  /* the codepoint sent with DOWN (UP repeats it) */
};

struct kbd {
    uint8_t  mods;           /* modifier byte of the last report taken */
    uint8_t  leds;           /* LED_* */
    uint8_t  nheld;
    struct kbd_held held[6];
    bool     repeating;      /* the most recent repeatable key is still down */
    uint8_t  rep_raw;
    uint16_t rep_usage;
    uint64_t rep_next;       /* its next REPEAT (uptime ns) */
    uint64_t rollover;       /* phantom (ErrorRollOver) reports ignored */
    uint64_t short_reports;  /* reports too short to be a boot report */
};

struct hid {
    handle_t usb, input, reports, port;
    uint16_t vendor, product;
    uint8_t  iface, alt, subclass, protocol, speed;
    uint8_t  ep_in, interval_ms;
    uint16_t max_packet;
    uint16_t report_desc_len;
    enum hid_kind kind;
    enum hid_stop stop;
    uint64_t nreports, events, input_errors, led_errors;
    uint64_t keys_down;      /* key DOWN events (for the RESULTS line at the end) */
    uint8_t  mouse_buttons;
    struct kbd kbd;
    uint8_t *buf;            /* 1024 bytes: descriptors, reports */
};

/* hid.c: send one event to the console (or to the log without DR_INPUT);
 * sets h->stop when the console is gone. */
void hid_key(struct hid *h, uint16_t usage, uint8_t state, uint8_t mods, uint32_t codepoint);
/* hid.c: SET_REPORT(output) with the LED byte. */
void hid_set_leds(struct hid *h, uint8_t leds);

/* keyboard.c */
void     kbd_init(struct hid *h);
void     kbd_report(struct hid *h, const uint8_t *r, uint32_t n, uint64_t now);
uint64_t kbd_repeat_deadline(const struct hid *h);
void     kbd_repeat(struct hid *h, uint64_t now);
