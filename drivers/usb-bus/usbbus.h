/* usb-bus internals, shared by its files: the xHCI host controller
 * (hc.c), the USB device model (devices.c: the device table and contexts;
 * control.c: control transfers and descriptors; intr.c: interrupt-IN
 * endpoints; config.c: configurations and interfaces; report.c: log and
 * RESULTS lines), enumeration (attach.c), hubs (hub.c), root ports
 * (rootport.c), the port work the main loop drives (work.c), and the
 * servers plus the main loop (serve.c). See serve.c for the overview.
 * Only <jam/driver.h> and the generated IDL headers are included, like
 * every driver. */
#pragma once

#include <jam/driver.h>

#define NS_PER_US 1000ull
#define NS_PER_MS 1000000ull
#define PAGE      4096u

static inline void zero(void *p, uint64_t n) { __builtin_memset(p, 0, n); }
static inline void copy(void *d, const void *s, uint64_t n) { __builtin_memcpy(d, s, n); }
static inline uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* ---- limits ---------------------------------------------------------------- */

#define MAX_DEVS     48     /* devices (slots) we track, hubs included */
#define MAX_IFS      8      /* interfaces per device (active alternate settings) */
#define MAX_EPS_IF   8      /* endpoints per interface we remember */
#define MAX_LEVEL    6      /* a device on a root port is level 1; 5 hub tiers below */
#define POOL_PAGES   320    /* DMA pages for contexts, rings and buffers */
#define INTR_TRBS    8      /* interrupt-IN transfers kept queued per endpoint */
#define MAX_CHANS    96     /* served channels (interfaces and report channels) */
#define CFG_MAX      4096   /* biggest configuration descriptor we read */

/* ---- xHCI registers (xHCI 1.2, chapter 5) ---------------------------------- */

#define CAP_CAPLENGTH   0x00
#define CAP_HCSPARAMS1  0x04
#define CAP_HCSPARAMS2  0x08
#define CAP_HCCPARAMS1  0x10
#define CAP_DBOFF       0x14
#define CAP_RTSOFF      0x18

#define OP_USBCMD   0x00
#define OP_USBSTS   0x04
#define OP_PAGESIZE 0x08
#define OP_CRCR     0x18
#define OP_DCBAAP   0x30
#define OP_CONFIG   0x38
#define OP_PORTSC(p) (0x400 + 0x10 * ((p) - 1))   /* p from 1 */

#define CMD_RS    (1u << 0)
#define CMD_HCRST (1u << 1)
#define CMD_INTE  (1u << 2)
#define CMD_HSEE  (1u << 3)

#define STS_HCH  (1u << 0)
#define STS_HSE  (1u << 2)
#define STS_EINT (1u << 3)
#define STS_PCD  (1u << 4)
#define STS_CNR  (1u << 11)
#define STS_HCE  (1u << 12)

#define CRCR_RCS (1u << 0)
#define CRCR_CA  (1u << 2)
#define CRCR_CRR (1u << 3)

#define IR0        0x20
#define IR_IMAN    0x00
#define IR_IMOD    0x04
#define IR_ERSTSZ  0x08
#define IR_ERSTBA  0x10
#define IR_ERDP    0x18
#define IMAN_IP    (1u << 0)
#define IMAN_IE    (1u << 1)
#define ERDP_EHB   (1u << 3)
#define IMOD_40US  160u

/* PORTSC */
#define PS_CCS   (1u << 0)
#define PS_PED   (1u << 1)    /* RW1C: writing 1 DISABLES the port */
#define PS_OCA   (1u << 3)
#define PS_PR    (1u << 4)
#define PS_PLS(v) (((v) >> 5) & 0xf)
#define PS_PP    (1u << 9)
#define PS_SPEED(v) (((v) >> 10) & 0xf)
#define PS_LWS   (1u << 16)
#define PS_CSC   (1u << 17)
#define PS_PEC   (1u << 18)
#define PS_WRC   (1u << 19)
#define PS_OCC   (1u << 20)
#define PS_PRC   (1u << 21)
#define PS_PLC   (1u << 22)
#define PS_CEC   (1u << 23)
#define PS_WPR   (1u << 31)
#define PS_CHANGES (PS_CSC | PS_PEC | PS_WRC | PS_OCC | PS_PRC | PS_PLC | PS_CEC)
/* The bits written back unchanged (RWS: power, indicator, wake enables);
 * everything else written 0 so nothing RW1C is cleared by accident. */
#define PS_KEEP  (PS_PP | (3u << 14) | (7u << 25))

#define PLS_U0       0
#define PLS_U3       3
#define PLS_DISABLED 4
#define PLS_RXDETECT 5
#define PLS_INACTIVE 6
#define PLS_POLLING  7
#define PLS_COMPLIANCE 10

/* extended capabilities */
#define XCAP_LEGACY    1
#define XCAP_PROTOCOL  2
#define LEG_BIOS_OWNED (1u << 16)
#define LEG_OS_OWNED   (1u << 24)
#define LEGCTL_SMI_ENABLES 0x0000e011u
#define LEGCTL_SMI_STATUS  0xe0000000u

/* ---- TRBs ------------------------------------------------------------------ */

#define TRB_NORMAL      1
#define TRB_SETUP       2
#define TRB_DATA        3
#define TRB_STATUS      4
#define TRB_LINK        6
#define TRB_ENABLE_SLOT 9
#define TRB_DISABLE_SLOT 10
#define TRB_ADDRESS_DEV 11
#define TRB_CONFIG_EP   12
#define TRB_EVAL_CTX    13
#define TRB_RESET_EP    14
#define TRB_STOP_EP     15
#define TRB_SET_TR_DEQ  16
#define TRB_NOOP_CMD    23
#define TRB_TRANSFER_EV 32
#define TRB_CMD_DONE_EV 33
#define TRB_PORT_EV     34
#define TRB_HC_EV       37

#define TRB_C           (1u << 0)
#define TRB_TC          (1u << 1)
#define TRB_ISP         (1u << 2)
#define TRB_IOC         (1u << 5)
#define TRB_IDT         (1u << 6)
#define TRB_DIR_IN      (1u << 16)
#define TRB_TYPE(t)     ((uint32_t)(t) << 10)
#define TRB_TYPE_OF(c)  (((c) >> 10) & 0x3f)

/* completion codes */
#define CC_SUCCESS      1
#define CC_DATA_BUFFER  2
#define CC_BABBLE       3
#define CC_TRANSACTION  4
#define CC_TRB          5
#define CC_STALL        6
#define CC_RESOURCE     7
#define CC_BANDWIDTH    8
#define CC_NO_SLOTS     9
#define CC_SHORT_PACKET 13
#define CC_PARAMETER    17
#define CC_CONTEXT_STATE 19
#define CC_RING_STOPPED 24
#define CC_ABORTED      25
#define CC_STOPPED      26
#define CC_STOPPED_LEN  27
#define CC_STOPPED_SHORT 28
/* ours, never the controller's */
#define CC_TIMEOUT      256
#define CC_GONE         257
#define CC_BAD_SLOT     258   /* Enable Slot "succeeded" with a slot id outside 1..MaxSlotsEn */

#define RING_TRBS 256   /* one 4 KiB segment; the last is the Link TRB */

struct trb {
    uint32_t d0, d1, d2, d3;
};

/* USB speeds (the xHCI default Protocol Speed IDs; usb.idl's too) */
#define SPEED_FULL  1
#define SPEED_LOW   2
#define SPEED_HIGH  3
#define SPEED_SUPER 4
#define SPEED_SUPERPLUS 5

/* endpoint context types */
#define EPT_CONTROL  4
#define EPT_INTR_IN  7

/* ---- the controller -------------------------------------------------------- */

struct ring {
    volatile struct trb *t;
    uint64_t dev;       /* device address of t[0] */
    uint32_t enq;
    uint32_t cycle;
    int      page;      /* pool page, -1: none */
};

#define MAX_MAPS 48
#define MAX_PROTOS 8

struct hc {
    const char *name;
    handle_t dev, bar, irq, dma, port, serve;
    struct { uint32_t page; volatile uint8_t *va; } map[MAX_MAPS];
    unsigned nmap;
    bool map_failed;
    uint32_t map_fail_off;
    status_t map_fail_st;

    uint16_t vendor, device;
    uint8_t revision;
    bool msix;
    uint32_t irq_vectors;
    uint32_t caplen, hciver, hcs1, hcs2, hcc1, dboff, rtsoff;
    uint32_t ports, slots, scratchpads, pagesize, max_slots_en;
    uint32_t csz;                    /* context size: 32 or 64 */
    const char *handoff;
    /* Supported Protocol capabilities: port ranges */
    struct { uint8_t major, minor, first, count, slot_type, psic; } proto[MAX_PROTOS];
    unsigned nproto;

    /* the fixed DMA area: DCBAA, ERST, command and event rings, scratchpad array */
    handle_t ctx_vmo, sp_vmo;
    uint8_t *ctx;
    uint64_t ctx_dev;
    uint32_t ctx_pages;
    uint64_t ctx_pin, sp_pin;
    bool ctx_pinned, sp_pinned;

    /* the page pool */
    handle_t pool_vmo;
    uint8_t *pool;
    uint64_t *pool_addr;
    uint64_t pool_pin;
    bool pool_pinned;
    uint8_t pool_used[POOL_PAGES];
    uint32_t pool_inuse, pool_peak;

    uint32_t cmd_enq, cmd_cycle;
    uint32_t ev_deq, ev_cycle;
    bool running;
    bool dead;                       /* HSE / HCE, or a stuck command ring */

    /* the one outstanding command */
    struct {
        bool busy, done;
        uint64_t trb;
        uint32_t cc, slot, param;
    } cmd;

    /* the one outstanding control transfer */
    struct {
        bool busy, done;
        uint8_t slot;
        uint64_t data_trb, status_trb, setup_trb;
        uint32_t cc, len, residual;
        bool short_seen;
    } ctl;
    int ctl_page;                    /* the shared control bounce buffer */

    uint32_t port_changed[8];        /* bitmap of root ports with a Port Status Change event */
    bool stopping;                   /* DR_SERVE closed: wind down */
    bool serve_pending;
    uint64_t irqs, events, spurious_events, polled_events;
};

extern struct hc g_hc;

/* ---- the device model ------------------------------------------------------ */

struct ep {
    uint8_t dci;          /* 0: unused */
    uint8_t addr;         /* bEndpointAddress */
    uint8_t attr;         /* bmAttributes */
    uint8_t type;         /* EPT_* */
    uint8_t binterval;
    uint8_t ifnum;
    uint16_t mps;         /* wMaxPacketSize (bits 10:0) */
    uint8_t burst;        /* additional transactions (HS) or SS bMaxBurst */
    uint16_t esit;        /* max ESIT payload */
    uint8_t interval;     /* xHCI Interval field: 2^n x 125 us */
    bool configured;
    struct ring ring;
    /* interrupt IN polling */
    bool open;
    bool halted;
    uint8_t owner;        /* EP_OWNER_* */
    int chan;             /* reports channel slot (EP_OWNER_CLIENT), -1 none */
    int buf_page;
    uint8_t *buf;
    uint64_t buf_dev;
    /* queued transfers: TRB index in the ring -> buffer slot */
    struct { uint16_t idx; uint8_t slot; } inflight[INTR_TRBS];
    uint8_t ninflight;
    uint32_t errors_in_row;
    uint16_t last_cc;     /* the completion code that halted it */
    uint64_t reports, dropped, errors;
};

#define EP_OWNER_CLIENT 1
#define EP_OWNER_HUB    2

struct iface {
    uint8_t number, alt, cls, sub, proto, nep;
    uint8_t ep_addr[MAX_EPS_IF];
    uint8_t num_alts;
    int devmgr_chan;      /* the channel sent to devmgr, -1 none */
};

struct usbdev {
    bool used;
    bool gone;
    bool configured;      /* SET_CONFIGURATION done */
    bool reported;        /* its line is out */
    uint32_t id;          /* unique for this run; 0 = never */
    uint8_t slot;
    int parent;           /* devs[] index of its hub, -1: root port */
    uint8_t port;         /* port on the parent hub, or the root port */
    uint8_t root_port;
    uint8_t level;        /* 1: on a root port */
    uint32_t route;
    uint8_t speed;
    uint8_t tt_slot, tt_port;
    bool tt_mtt;
    uint32_t tt_clears;              /* CLEAR_TT_BUFFERs sent (the first few logged) */
    char path[24];

    int out_page, in_page;
    struct ring ep0;
    uint16_t mps0;
    uint8_t address;

    uint16_t vid, pid, bcd;
    uint8_t cls, sub, proto, nconfigs, cfg_value, iserial;
    uint8_t *cfg;         /* the active configuration descriptor (drv_malloc) */
    uint16_t cfg_len;
    char product[40];
    char serial[24];

    struct iface ifs[MAX_IFS];
    uint8_t nifs;
    struct ep eps[32];    /* by DCI */
    uint8_t max_dci;

    /* hub */
    bool is_hub, ss_hub;
    uint8_t hub_ports;
    uint8_t ttt;
    bool hub_mtt;
    uint16_t hub_chars;
    uint32_t pgood_ms;
    uint32_t hub_change[8];          /* ports (bit 0: the hub itself) to look at */
    bool hub_scan_all;
    uint8_t hub_intr_dci;

    const char *problem;             /* why it stopped short of configured */
    uint8_t port_fail[16];           /* hub: failed attach attempts per port */
    uint8_t port_oc[16];             /* hub: port power restores after over-current */
    uint32_t ep_recover;             /* DCIs to reset after an error (main loop) */
    uint32_t ep_drop;                /* DCIs whose client went away (main loop) */
};

extern struct usbdev *g_devs;   /* MAX_DEVS of them (drv_malloc) */

/* ---- hc.c ------------------------------------------------------------------ */

int  hc_bring_up(struct hc *h);
int  hc_shutdown(struct hc *h);
void hc_release(struct hc *h, bool quiet);
uint32_t hc_rd(struct hc *h, uint32_t off);
void hc_wr(struct hc *h, uint32_t off, uint32_t v);
uint32_t hc_portsc(struct hc *h, uint32_t port);
void hc_portsc_write(struct hc *h, uint32_t port, uint32_t set);
bool hc_port_is_usb3(struct hc *h, uint32_t port);
const char *cc_str(uint32_t cc);

int   pool_alloc(struct hc *h);            /* a zeroed page, -1 if none */
void  pool_free(struct hc *h, int page);
void *pool_va(struct hc *h, int page);
uint64_t pool_dev(struct hc *h, int page);

bool ring_init(struct hc *h, struct ring *r);
void ring_free(struct hc *h, struct ring *r);
/* Queue one TRB (d3 without the cycle bit); its device address. */
uint64_t ring_push(struct ring *r, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3);
uint32_t ring_index(const struct ring *r, uint64_t trb_dev);   /* RING_TRBS if not in it */

/* Run one command; its completion code (CC_TIMEOUT: none within timeout). */
uint32_t hc_command(struct hc *h, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3,
                    uint32_t *slot_out, uint64_t timeout_ms);
void hc_doorbell(struct hc *h, uint32_t slot, uint32_t target);
/* Process the event ring (and the interrupt). Called from every wait. */
void hc_poll(struct hc *h);
/* Wait for an interrupt (or anything else on the port) until deadline,
 * then poll. Packets for other keys are passed to serve_packet(). */
void hc_wait(struct hc *h, uint64_t deadline);
void hc_wait_idle(struct hc *h, uint64_t deadline);
/* Sleep that keeps servicing the controller. */
void hc_sleep(struct hc *h, uint64_t ms);
void hc_set_dcbaa(struct hc *h, uint32_t slot, uint64_t addr);
#define KEY_IRQ   0x7a60
#define KEY_SERVE 0x5e7e
#define KEY_CHAN  (1ull << 40)   /* | gen << 8 (16 bits) | index (8 bits) */

/* ---- devices.c ------------------------------------------------------------- */

extern uint32_t g_generation;             /* bumps on every attach and detach */
extern uint64_t g_last_change_ns;
extern uint32_t g_attached, g_detached, g_failed, g_report_generation;
extern bool g_first_report_done;
void devices_reset(void);                 /* the counters and ids, for a fresh start */
struct usbdev *dev_by_slot(uint8_t slot);
int  dev_index(const struct usbdev *d);
struct usbdev *child_at(int parent, uint8_t port);   /* parent -1: a root port */
struct usbdev *dev_alloc(void);           /* a cleared entry, NULL if all are used */
/* Give d's entry back. slot_disabled false: the controller may still own
 * the slot, so its DMA pages are kept (leaked) rather than reused. */
void dev_free(struct usbdev *d, bool slot_disabled);
bool disable_slot(struct usbdev *d);      /* true once the controller let go of the slot */
bool disable_slot_id(const char *path, uint32_t slot);   /* the same for a bare slot id */
volatile uint32_t *in_ctx(struct usbdev *d, unsigned index);    /* 0 control, 1 slot, dci+1 */
volatile uint32_t *out_ctx(struct usbdev *d, unsigned index);   /* 0 slot, dci */
void in_reset(struct usbdev *d);          /* input context: cleared, slot copied from output */
uint64_t in_dev(struct usbdev *d);        /* the input context's device address */

/* ---- control.c ------------------------------------------------------------- */

/* A control transfer on d's endpoint 0. *actual gets the bytes moved.
 * Returns a completion code: CC_SUCCESS (short packets included),
 * CC_STALL (recovered), CC_TIMEOUT, CC_GONE or another error. */
uint32_t usb_control(struct usbdev *d, uint8_t rt, uint8_t req, uint16_t value, uint16_t index,
                     uint16_t length, void *data, uint32_t *actual, uint64_t timeout_ms);
void ctl_event(struct hc *h, uint64_t trb, uint32_t cc, uint32_t residual);
/* Endpoint recovery: Reset Endpoint (tsp: keep the data toggle), or Stop
 * Endpoint; then the dequeue pointer moves past what was queued. */
void ep_reset_tsp(struct usbdev *d, uint8_t dci, struct ring *r, bool tsp);
void ep_stop(struct usbdev *d, uint8_t dci, struct ring *r);
/* GET_DESCRIPTOR, tried up to three times. */
uint32_t get_desc(struct usbdev *d, uint8_t type, uint8_t index, uint16_t lang, void *buf,
                  uint16_t len, uint32_t *actual);
void get_string(struct usbdev *d, uint8_t index, uint16_t lang, char *out, unsigned cap);

/* ---- intr.c ---------------------------------------------------------------- */

int  ep_open_intr(struct usbdev *d, struct ep *e, uint8_t owner, int chan);
void ep_close(struct usbdev *d, struct ep *e);
void usb_transfer_event(struct hc *h, uint8_t slot, uint8_t dci, uint64_t trb, uint32_t cc,
                        uint32_t residual);
bool intr_upkeep(struct hc *h);           /* halted and dropped endpoints; true if any */

/* ---- config.c -------------------------------------------------------------- */

void parse_config(struct usbdev *d);
struct iface *usb_iface(struct usbdev *d, uint8_t number);
/* Configure Endpoint: add / drop the interrupt-IN endpoints in the DCI
 * bitmaps, with the slot's Context Entries and hub fields. */
uint32_t configure_eps(struct usbdev *d, uint32_t add, uint32_t drop);
uint32_t dev_set_interface(struct usbdev *d, struct iface *f, uint8_t alt);

/* ---- report.c -------------------------------------------------------------- */

void usb_counts(uint32_t *devices, uint32_t *hubs, uint32_t *ifaces, uint32_t *hid,
                uint32_t *problems);
void usb_report_all(bool at_stop);        /* a RESULTS line per device not listed yet + the summary */
void usb_report_summary(const char *when);
void dev_line(struct usbdev *d, bool report_it, const char *prefix);   /* report_it: RESULTS */
void dev_log_detail(struct usbdev *d);
void dev_set_path(struct usbdev *d, const struct usbdev *parent, uint8_t port);   /* "9.1" */
const char *speed_long(uint8_t speed);

/* ---- attach.c -------------------------------------------------------------- */

/* Enumerate the device just reset on `port` of hub `parent` (-1: a root
 * port). True if it ended configured. */
bool enumerate(int parent, uint8_t port, uint8_t speed);
void detach(struct usbdev *d, const char *why, bool quiet);   /* and everything below it */

/* ---- hub.c, rootport.c ----------------------------------------------------- */

/* Over-current: the port's power went off (the hub or the controller cut
 * it). After a 100 ms cool-down it is powered again if the condition has
 * cleared, at most OC_RESTORES times per port per boot, so a device that
 * keeps shorting stays off instead of cycling. Each step is logged; the
 * device reconnects by itself and is enumerated as usual. */
#define OC_RESTORES 3

bool hub_setup(struct usbdev *d);         /* after SET_CONFIGURATION; false: not used as a hub */
void hub_work(struct usbdev *hub);        /* one unit: the hub's own change, or one port */
void root_port(struct hc *h, uint32_t p);
void root_ports_reset(void);

/* ---- work.c ---------------------------------------------------------------- */

bool usb_work(struct hc *h);              /* pending port and hub work; true if any was done */
void usb_reset_state(void);
void usb_start(struct hc *h);             /* the first scan of every root port */
void usb_stop_all(struct hc *h);          /* shutdown: every device detached quietly */
bool usb_busy(void);                      /* port or hub work pending */

/* ---- serve.c --------------------------------------------------------------- */

void serve_packet(struct hc *h, const struct port_packet *p);
/* Channels: interface channels (the usb protocol) and report channels. */
int  chan_add(handle_t h, uint8_t kind, uint32_t dev_id, uint8_t a);
void chan_close(int i);
handle_t chan_handle(int i);
#define CHAN_IFACE   1
#define CHAN_REPORTS 2
void serve_iface_gone(uint32_t dev_id);   /* close every channel of a device */
void serve_device_ready(struct usbdev *d); /* tell devmgr about its interfaces */
/* A report from an interrupt IN endpoint for a client: returns false if the
 * client's channel is gone (the caller closes the endpoint). */
bool serve_report(int chan, const void *data, uint32_t len, bool *dropped);
