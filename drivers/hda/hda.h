/* hda: the Intel High Definition Audio driver's own pieces (drv/hda).
 *
 * The driver resets the controller, finds the codecs on the link and
 * prints each codec's widget graph, then serves abi/idl/hda.idl (the
 * dump again, and one output stream) until devmgr closes its channel. It
 * sends the codecs GET verbs (hda_get refuses anything else) and, for an
 * open stream, a converter's format and stream tag (hda_converter_set);
 * nothing it does changes routing, gains, pin controls, EAPD or power
 * states, so no sound reaches a jack yet.
 *
 * Files: main.c (start, the protocol, exit), ctrl.c (the controller:
 * reset, the CORB/RIRB command rings, the immediate command interface,
 * stop), graph.c (reading a codec's nodes into struct codec), dump.c
 * (the readable lines), stream.c (the output stream), irq.c (the loop:
 * the channels and the MSI).
 *
 * Register offsets, bits and verbs are from the Intel High Definition
 * Audio Specification, revision 1.0a (2010): chapter 3 (controller
 * registers), 4 (programming model), 7 (codecs: verbs and parameters in
 * 7.3). Each group below names its register, verb or parameter as the
 * spec does, so it can be looked up there. */
#pragma once

#include <jam/driver.h>

/* ---- controller registers (spec chapter 3) ----------------------------------- */

#define HDA_GCAP       0x00   /* 16: bit 0 64-bit OK, 2:1 SDOs, 7:3 BSS, 11:8 ISS, 15:12 OSS */
#define HDA_VMIN       0x02   /* 8 */
#define HDA_VMAJ       0x03   /* 8 */
#define HDA_GCTL       0x08   /* 32 */
#define HDA_WAKEEN     0x0c   /* 16 */
#define HDA_STATESTS   0x0e   /* 16: bit n = a codec asked for attention on SDI n (RW1C) */
#define HDA_INTCTL     0x20   /* 32: bit 31 GIE, bit 30 CIE, bits 29:0 stream enables */
#define HDA_INTSTS     0x24   /* 32 */
#define HDA_CORBLBASE  0x40
#define HDA_CORBUBASE  0x44
#define HDA_CORBWP     0x48   /* 16: bits 7:0 */
#define HDA_CORBRP     0x4a   /* 16: bits 7:0; bit 15 reset */
#define HDA_CORBCTL    0x4c   /* 8 */
#define HDA_CORBSTS    0x4d   /* 8 */
#define HDA_CORBSIZE   0x4e   /* 8: bits 1:0 size, 7:4 sizes supported */
#define HDA_RIRBLBASE  0x50
#define HDA_RIRBUBASE  0x54
#define HDA_RIRBWP     0x58   /* 16: bits 7:0; bit 15 reset (write only) */
#define HDA_RINTCNT    0x5a   /* 16: responses per interrupt */
#define HDA_RIRBCTL    0x5c   /* 8 */
#define HDA_RIRBSTS    0x5d   /* 8 (RW1C) */
#define HDA_RIRBSIZE   0x5e   /* 8 */
#define HDA_ICOI       0x60   /* 32: immediate command out */
#define HDA_ICII       0x64   /* 32: immediate response in */
#define HDA_ICIS       0x68   /* 16: immediate command status */
#define HDA_DPLBASE    0x70   /* DMA position buffer */
#define HDA_DPUBASE    0x74
#define HDA_SD_BASE    0x80   /* stream descriptor n at 0x80 + 0x20 * n */
#define HDA_SD_STRIDE  0x20
#define HDA_SD_CTL     0x00   /* 24 bits (read as 32 with STS in the top byte) */

#define GCTL_CRST      (1u << 0)   /* 0: the controller and the link in reset */
#define GCTL_UNSOL     (1u << 8)   /* accept unsolicited responses */
#define CORBRP_RST     (1u << 15)
#define CORBCTL_RUN    (1u << 1)
#define RIRBWP_RST     (1u << 15)
#define RIRBCTL_RINTCTL (1u << 0)  /* set RINTFL every RINTCNT responses */
#define RIRBCTL_DMAEN  (1u << 1)
#define RIRBSTS_RINTFL (1u << 0)
#define RIRBSTS_OIS    (1u << 2)
#define ICIS_ICB       (1u << 0)   /* busy: a command is out */
#define ICIS_IRV       (1u << 1)   /* a response is in ICII (RW1C) */
#define SDCTL_RUN      (1u << 1)

#define HDA_MAX_CODECS 15          /* STATESTS bits 14:0 */
#define HDA_MAX_STREAMS 30         /* ISS + OSS + BSS, each at most 15 */

/* ---- verbs (spec 7.3) ------------------------------------------------------
 * A command: bits 31:28 codec address, 26:20 node id, then either a
 * 12-bit verb and an 8-bit payload, or a 4-bit verb and a 16-bit payload.
 * Only the GET verbs are here: this driver never sends another kind. */

#define V_GET_PARAM      0xf00   /* payload: parameter id */
#define V_GET_CONN_SEL   0xf01
#define V_GET_CONN_LIST  0xf02   /* payload: index of the first entry wanted */
#define V_GET_POWER      0xf05   /* 7:4 actual, 3:0 set */
#define V_GET_STREAM     0xf06   /* 7:4 stream, 3:0 channel */
#define V_GET_PIN_CTL    0xf07
#define V_GET_UNSOL      0xf08   /* bit 7 enabled, 5:0 tag */
#define V_GET_PIN_SENSE  0xf09   /* bit 31 presence */
#define V_GET_EAPD       0xf0c   /* bit 1 EAPD, bit 0 BTL, bit 2 L/R swap */
#define V_GET_SUBSYS     0xf20
#define V_GET_CONFIG     0xf1c   /* the pin's configuration default */
#define V4_GET_FORMAT    0xa     /* 4-bit verb: the converter's stream format */
#define V4_GET_AMP       0xb     /* 4-bit verb: payload bit 15 output, 13 left, 3:0 index */

#define AMP_GET_OUT      (1u << 15)
#define AMP_GET_LEFT     (1u << 13)

/* Parameters (spec 7.3, Get Parameter). */
#define P_VENDOR         0x00
#define P_REVISION       0x02
#define P_NODES          0x04    /* 23:16 first node, 7:0 count */
#define P_FG_TYPE        0x05    /* 7:0 type (1 audio), bit 8 unsolicited capable */
#define P_AFG_CAPS       0x08
#define P_WIDGET_CAPS    0x09
#define P_PCM            0x0a    /* 20:16 sizes, 11:0 rates */
#define P_FORMATS        0x0b
#define P_PIN_CAPS       0x0c
#define P_AMP_IN         0x0d
#define P_CONN_LEN       0x0e    /* 6:0 length, bit 7 long form */
#define P_POWER_STATES   0x0f
#define P_GPIO           0x11
#define P_AMP_OUT        0x12

#define FG_AUDIO         0x01

/* Audio Widget Capabilities (spec 7.3, parameters). */
#define WCAP_TYPE(c)     (((c) >> 20) & 0xf)
#define WCAP_STEREO      (1u << 0)
#define WCAP_IN_AMP      (1u << 1)
#define WCAP_OUT_AMP     (1u << 2)
#define WCAP_AMP_OVR     (1u << 3)
#define WCAP_FMT_OVR     (1u << 4)
#define WCAP_UNSOL       (1u << 7)
#define WCAP_CONN_LIST   (1u << 8)
#define WCAP_DIGITAL     (1u << 9)
#define WCAP_POWER       (1u << 10)
#define WCAP_CHANS(c)    (((((c) >> 13) & 7u) << 1 | ((c) & 1u)) + 1)

enum wtype {
    W_OUT = 0, W_IN = 1, W_MIXER = 2, W_SELECTOR = 3, W_PIN = 4, W_POWER = 5,
    W_KNOB = 6, W_BEEP = 7, W_VENDOR = 0xf,
};

/* Pin Capabilities (spec 7.3, parameters). */
#define PINCAP_IMPEDANCE (1u << 0)
#define PINCAP_TRIGGER   (1u << 1)
#define PINCAP_PRESENCE  (1u << 2)
#define PINCAP_HP        (1u << 3)
#define PINCAP_OUT       (1u << 4)
#define PINCAP_IN        (1u << 5)
#define PINCAP_BALANCED  (1u << 6)
#define PINCAP_HDMI      (1u << 7)
#define PINCAP_VREF(c)   (((c) >> 8) & 0xffu)
#define PINCAP_EAPD      (1u << 16)
#define PINCAP_DP        (1u << 24)

/* Configuration default bit 8: the jack has no presence detection. */
#define CFG_NO_PRESENCE  (1u << 8)

/* ---- the controller ----------------------------------------------------------- */

#define HDA_CMD_TIMEOUT  (100 * NS_PER_MS)   /* one verb's answer */

struct hda {
    volatile void *regs;       /* BAR 0, mapped uncached */
    handle_t dma;              /* DR_DMA */
    handle_t ring_vmo;         /* one DMA32 page: CORB at 0, RIRB at RIRB_OFF */
    uint64_t ring_pin;         /* its pin id; ring_pinned says whether it is held */
    bool     ring_pinned;
    volatile uint32_t *corb;   /* the CORB, mapped */
    volatile uint64_t *rirb;   /* the RIRB, mapped */
    uint32_t corb_entries;     /* 2, 16 or 256 */
    uint32_t rirb_entries;
    uint32_t corb_wp;          /* the last entry written */
    uint32_t rirb_rp;          /* the last entry read */
    bool     rings;            /* commands go through CORB/RIRB (else the immediate interface) */
    bool     immediate_ok;     /* the immediate interface answered (fallback) */
    uint16_t gcap;             /* GCAP as read after reset */
    uint16_t codec_mask;       /* STATESTS after reset: the codecs present */
    uint32_t unsol;            /* unsolicited responses seen (none expected: UNSOL off) */
    uint32_t timeouts;         /* verbs that got no answer */
    uint16_t vendor, device;   /* the PCI ids */
    uint32_t cfg40[4];         /* PCI config 0x40-0x4f: Intel's vendor registers (TCSEL at
                                * 0x44, clock gating nearby), logged for the PC's dump */
};

/* ctrl.c. Map BAR 0, stop every DMA engine, reset the controller and the
 * link, turn bus mastering on, find the codecs (STATESTS) and start the
 * command rings (the immediate interface if they don't answer). The steps
 * log what went wrong; ERR_* then. */
status_t hda_ctrl_start(struct hda *h, handle_t bar);
/* One GET verb (12-bit `verb` with an 8-bit payload, or 4-bit `verb` with
 * a 16-bit payload) to node `nid` of codec `cad`; *out: the response.
 * ERR_INVALID_ARGS for anything but a GET verb (this driver sets
 * nothing), ERR_TIMED_OUT if the codec does not answer in
 * HDA_CMD_TIMEOUT. */
status_t hda_get(struct hda *h, unsigned cad, unsigned nid, uint32_t verb, uint32_t payload,
                 uint32_t *out);
/* GET_PARAMETER: hda_get(V_GET_PARAM, param). */
status_t hda_param(struct hda *h, unsigned cad, unsigned nid, uint32_t param, uint32_t *out);
/* Stop the rings, put the controller back in reset, unpin the ring page.
 * Safe to call more than once and after a failed start. */
void     hda_ctrl_stop(struct hda *h);

/* ---- a codec's graph (graph.c) --------------------------------------------- */

#define MAX_WIDGETS  96    /* nodes per function group we keep (a Realtek ALC has ~40) */
#define MAX_CONN     32    /* connection list entries we keep per widget */
#define MAX_IN_AMPS  10    /* input amp values read per widget */

struct amp_now {
    uint8_t l, r;          /* bit 7 mute, 6:0 gain */
};

struct widget {
    uint8_t  nid;
    uint32_t caps;               /* audio widget capabilities */
    uint32_t pincaps;            /* W_PIN: pin capabilities */
    uint32_t config;             /* W_PIN: configuration default */
    uint32_t amp_in, amp_out;    /* amplifier capabilities (the AFG's unless overridden) */
    uint32_t pcm, formats;       /* converters: sizes/rates and formats (AFG's unless overridden) */
    uint8_t  nconn;              /* connection list length (entries kept: <= MAX_CONN) */
    bool     conn_long;          /* long-form list */
    uint16_t conn[MAX_CONN];     /* the connection list (a range already expanded) */
    uint8_t  conn_sel;           /* the current connection (selectors, pins, converters) */
    bool     has_sel;            /* conn_sel was read */
    uint8_t  pin_ctl;            /* W_PIN: pin widget control */
    uint8_t  eapd;               /* W_PIN with EAPD: EAPD/BTL */
    uint32_t sense;              /* W_PIN with presence detect and no trigger */
    bool     has_sense;
    uint8_t  power;              /* power state (actual << 4 | set), if the widget has it */
    bool     has_power;
    uint8_t  unsol;              /* unsolicited response control (bit 7 on, 5:0 tag) */
    uint8_t  stream;             /* converters: stream << 4 | channel */
    uint16_t format;             /* converters: stream format */
    struct amp_now out;          /* current output amp */
    uint8_t  nin;                /* input amps read */
    struct amp_now in[MAX_IN_AMPS];
    uint32_t errors;             /* verbs that failed for this node */
};

struct codec {
    uint8_t  cad;                /* codec address */
    uint32_t vendor;             /* vendor << 16 | device */
    uint32_t revision;
    uint32_t subsys;             /* subsystem id (the board's) */
    uint8_t  afg;                /* the audio function group node, 0: none */
    uint8_t  afg_unsol;          /* the AFG is unsolicited-capable */
    uint8_t  nfgs;               /* function groups */
    uint32_t fg_types;           /* bit t: a function group of type t was seen */
    uint32_t afg_caps, afg_pcm, afg_formats, afg_amp_in, afg_amp_out, afg_states, afg_gpio;
    uint8_t  afg_power;
    uint8_t  first, count;       /* the AFG's widgets: nodes first..first+count-1 */
    uint32_t nw;                 /* widgets read (<= MAX_WIDGETS) */
    struct widget w[MAX_WIDGETS];
};

/* Read codec `cad`'s root node, its audio function group and every
 * widget under it into *c, with GET verbs only. ERR_TIMED_OUT: the codec
 * does not answer at all (a widget that fails is counted in its
 * `errors` and the walk goes on). */
status_t hda_read_codec(struct hda *h, unsigned cad, struct codec *c);

/* ---- the dump (dump.c) ---------------------------------------------------------
 * Lines go to the log (and, for hda.dump, into a text buffer). */

struct out {
    char    *buf;       /* the text so far, or NULL: log only */
    size_t   len, cap;  /* bytes in buf, its size */
    unsigned lines;     /* lines written */
    bool     log;       /* each line also to the log (paced: see dump.c) */
};

void out_line(struct out *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* The controller's line. */
void hda_dump_ctrl(struct out *o, const struct hda *h, const char *where);
/* One codec: its header lines, then a line per widget (pins get two). */
void hda_dump_codec(struct out *o, const struct codec *c);

/* ---- the output stream (stream.c) and the driver's loop (irq.c) -------------------
 * One output stream: the first output stream descriptor (index ISS, as
 * GCAP counts them), stream tag 1, 48 kHz 16-bit stereo, from a 64 KiB
 * DMA32 ring of 4 periods of 16 KiB. Its Buffer Descriptor List and the
 * DMA position buffer share one more DMA32 page. Spec chapter 3 (stream
 * descriptor registers, DPLBASE) and chapter 4 (stream setup). */

#define SD_CTL0        0x00   /* 8: bit 0 SRST, 1 RUN, 2 IOCE, 3 FEIE, 4 DEIE */
#define SD_CTL2        0x02   /* 8: bits 7:4 the stream tag (STRM) */
#define SD_STS         0x03   /* 8 (RW1C): bit 2 BCIS, 3 FIFOE, 4 DESE, 5 FIFORDY */
#define SD_LPIB        0x04   /* 32: link position in the cyclic buffer, bytes */
#define SD_CBL         0x08   /* 32: cyclic buffer length, bytes */
#define SD_LVI         0x0c   /* 16: last valid BDL index */
#define SD_FIFOS       0x10   /* 16: FIFO size, bytes */
#define SD_FMT         0x12   /* 16: stream format */
#define SD_BDPL        0x18   /* BDL address */
#define SD_BDPU        0x1c

#define SDCTL_SRST     (1u << 0)
#define SDCTL_IOCE     (1u << 2)   /* interrupt on completion of a buffer with IOC */
#define SDCTL_FEIE     (1u << 3)   /* FIFO error interrupt */
#define SDCTL_DEIE     (1u << 4)   /* descriptor error interrupt */
#define SDSTS_BCIS     (1u << 2)   /* a buffer with IOC completed */
#define SDSTS_FIFOE    (1u << 3)   /* FIFO under-run */
#define SDSTS_DESE     (1u << 4)   /* descriptor error */
#define INTCTL_GIE     (1u << 31)
#define INTCTL_CIE     (1u << 30)
#define INTSTS_CIS     (1u << 30)
#define DPLBASE_ENABLE (1u << 0)
#define PCI_TCSEL      0x44        /* Intel: bits 2:0 the traffic class of the controller's DMA */

#define V_SET_STREAM   0x706       /* payload: stream tag << 4 | lowest channel */
#define V4_SET_FORMAT  0x2         /* 4-bit verb: the converter's stream format */

#define STREAM_TAG     1u
#define STREAM_FORMAT  0x0011u     /* 48 kHz (base 48, x1, /1), 16-bit, 2 channels */
#define STREAM_RATE    48000u
#define FRAME_BYTES    4u
#define RING_BYTES     (64u * 1024)
#define PERIODS        4u
#define PERIOD_BYTES   (RING_BYTES / PERIODS)
#define PERIOD_NS      (PERIOD_BYTES / FRAME_BYTES * NS_PER_S / STREAM_RATE)
#define STALL_PERIODS  4u          /* no progress this long while running: stalled */

/* A contiguous DMA32 buffer, mapped and pinned. */
struct dma_buf {
    handle_t vmo;
    uint8_t *map;              /* mapped read-write, or NULL */
    uint64_t pin;              /* the pin's id, while `pinned` */
    bool     pinned;
    uint64_t addr;             /* the device address of its first byte */
};

struct stream {
    unsigned sd;               /* the stream descriptor's index; HDA_MAX_STREAMS: none */
    uint32_t sd_regs;          /* its registers: HDA_SD_BASE + sd * HDA_SD_STRIDE */
    handle_t dev;              /* DR_PCIDEV, for TCSEL (HANDLE_INVALID: not set) */
    bool     open;             /* a client has it */
    bool     running;          /* RUN set */
    unsigned cad, dac;         /* the output converter the stream feeds */
    struct dma_buf ring;       /* the samples, shared with the client */
    struct dma_buf page;       /* the BDL at 0, the DMA position buffer at POS_OFF */
    /* the position: the byte offset read last, bytes played since the
     * open, bytes zeroed behind the play position (the same after every
     * update), and when `played` last grew */
    uint32_t last_off;
    uint64_t played, cleared;
    uint64_t progress_ns;
    uint32_t iocs;             /* buffer-completion interrupts taken */
    uint32_t fifo_errors;      /* FIFOE/DESE seen */
    uint32_t lpib_diff_max;    /* largest gap between the position buffer and LPIB, bytes */
};

/* stream.c. Pick the output stream descriptor (none if GCAP has no
 * output streams); touches no register. */
void     stream_init(struct hda *h, struct stream *s, handle_t dev);
/* Open it (the checks and results of hda.idl's open_output): DMA
 * buffers, stream reset and setup, the converter's format and stream
 * tag, the stream's interrupt enabled. *ring: the client's handle. */
status_t stream_open(struct hda *h, struct stream *s, handle_t *ring);
status_t stream_start(struct hda *h, struct stream *s);
status_t stream_stop(struct hda *h, struct stream *s);
/* Read the position, count what played and zero the ring behind it. */
void     stream_update(struct hda *h, struct stream *s);
/* The stream's status bits, read and cleared (from the interrupt). */
void     stream_status(struct hda *h, struct stream *s);
/* Stop the DMA engine (RUN clear, waited for), reset the stream, point
 * the converter at no stream, release the buffers. Safe when not open. */
void     stream_close(struct hda *h, struct stream *s, const char *why);

/* ctrl.c. The converter's stream format (V4_SET_FORMAT) or stream tag
 * and channel (V_SET_STREAM), the only SET verbs this driver sends;
 * anything else: ERR_INVALID_ARGS. */
status_t hda_converter_set(struct hda *h, unsigned cad, unsigned nid, uint32_t verb,
                           uint32_t payload);
/* Wait (bounded, logged on a timeout) until (8-bit register reg & mask)
 * == want. ERR_TIMED_OUT. */
status_t hda_wait8(struct hda *h, uint32_t reg, uint8_t mask, uint8_t want, const char *what);

/* irq.c. Serve `ops` (ctx) on DR_SERVE, the output stream on the channels
 * open_output hands out, and the controller's MSI, until devmgr closes
 * DR_SERVE (OK) or a wait fails (its status); the stream is closed on
 * the way out. */
struct hda_ops;
status_t hda_loop(struct hda *h, const struct driver_start *ds, const struct hda_ops *ops,
                  void *ctx);
