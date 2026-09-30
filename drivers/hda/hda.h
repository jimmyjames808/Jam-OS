/* hda: the Intel High Definition Audio driver's own pieces (drv/hda).
 *
 * The driver resets the controller, finds the codecs on the link, prints
 * each codec's widget graph, picks the path from a DAC to the front-panel
 * headphone jack (path.c, checked at start against fixtures.c) and
 * programs it with every amplifier on it muted and the pin's output off;
 * then it serves abi/idl/hda.idl until devmgr closes its channel. It makes
 * no sound: the stream and the unmuting are not built yet. Every verb goes
 * through verbs.c, which takes GET verbs and an allow-list of SET verbs
 * (routing, amplifiers, pin control, EAPD, power, converter format and
 * stream, unsolicited enable, pin sense) and refuses anything else, so the
 * board's own settings (configuration defaults, GPIOs, vendor
 * coefficients) are never written.
 *
 * Files: main.c (start, the protocol, exit), ctrl.c (the controller:
 * reset, the CORB/RIRB command rings, the immediate command interface,
 * stop), verbs.c (the verbs the driver may send), graph.c (reading a
 * codec's nodes into struct codec), dump.c (the readable lines).
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
 * The SET verbs listed here are the only ones verbs.c lets through. */

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

#define V_SET_CONN_SEL   0x701   /* payload: connection list index */
#define V_SET_POWER      0x705   /* payload: D0-D3 */
#define V_SET_STREAM     0x706   /* 7:4 stream, 3:0 channel */
#define V_SET_PIN_CTL    0x707   /* PINCTL_* | vref */
#define V_SET_UNSOL      0x708   /* bit 7 enable, 5:0 tag */
#define V_SET_PIN_SENSE  0x709   /* starts a presence/impedance measurement; bit 0 right */
#define V_SET_EAPD       0x70c   /* bit 1 EAPD, bit 0 BTL, bit 2 L/R swap */
#define V4_SET_FORMAT    0x2     /* 4-bit verb: the converter's stream format */
#define V4_SET_AMP       0x3     /* 4-bit verb: AMP_SET_* | index << 8 | mute | gain */

#define AMP_SET_OUT      (1u << 15)
#define AMP_SET_IN       (1u << 14)
#define AMP_SET_LEFT     (1u << 13)
#define AMP_SET_RIGHT    (1u << 12)
#define AMP_SET_INDEX(i) ((uint32_t)(i) << 8)   /* 11:8 which input amp */
#define AMP_MUTE         (1u << 7)              /* in SET and GET values: 6:0 the gain step */

/* Pin Widget Control (spec 7.3, Pin Widget Control). */
#define PINCTL_HP        (1u << 7)   /* the headphone amp */
#define PINCTL_OUT       (1u << 6)
#define PINCTL_IN        (1u << 5)

#define PS_D0            0x0         /* power states: set in 3:0, actual in 7:4 */

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

/* Configuration Default (spec 7.3, Configuration Default): 31:30 port
 * connectivity, 29:24 location (5:4 gross, 3:0 side), 23:20 default
 * device, 7:4 association, 3:0 sequence; bit 8: no presence detection. */
#define CFG_CONN(c)      ((c) >> 30)
#define CFG_LOCATION(c)  (((c) >> 24) & 0x3fu)
#define CFG_DEVICE(c)    (((c) >> 20) & 0xfu)
#define CFG_ASSOC(c)     (((c) >> 4) & 0xfu)
#define CFG_SEQ(c)       ((c) & 0xfu)
#define CFG_NO_PRESENCE  (1u << 8)
#define CONN_JACK        0           /* CFG_CONN */
#define CONN_NONE        1
#define LOC_EXT_FRONT    0x02        /* CFG_LOCATION: external, front */
#define DEV_LINE_OUT     0x0         /* CFG_DEVICE */
#define DEV_SPEAKER      0x1
#define DEV_HP_OUT       0x2

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
/* Send one command word `cmd` (spec 7.3) to codec `cad` and wait for its
 * response in *out. ERR_TIMED_OUT (counted in h->timeouts) if it does not
 * come in HDA_CMD_TIMEOUT. The raw send, unchecked: only verbs.c calls
 * it, and everything else goes through verbs.c's checked calls. */
status_t hda_command(struct hda *h, unsigned cad, uint32_t cmd, uint32_t *out);
/* Stop the rings, put the controller back in reset, unpin the ring page.
 * Safe to call more than once and after a failed start. */
void     hda_ctrl_stop(struct hda *h);

/* ---- verbs (verbs.c) ---------------------------------------------------------- */

/* One GET verb (12-bit `verb` with an 8-bit payload, or 4-bit `verb` with
 * a 16-bit payload) to node `nid` of codec `cad`; *out: the response.
 * ERR_INVALID_ARGS for anything but a GET verb, ERR_TIMED_OUT if the
 * codec does not answer in HDA_CMD_TIMEOUT. */
status_t hda_get(struct hda *h, unsigned cad, unsigned nid, uint32_t verb, uint32_t payload,
                 uint32_t *out);
/* GET_PARAMETER: hda_get(V_GET_PARAM, param). */
status_t hda_param(struct hda *h, unsigned cad, unsigned nid, uint32_t param, uint32_t *out);
/* One SET verb from the allow-list (the V_SET_* and V4_SET_* above), its
 * payload checked against the bits that verb defines. ERR_NOT_SUPPORTED
 * for a verb not on the list (logged: a driver bug), ERR_INVALID_ARGS for
 * a payload with bits the verb does not define, ERR_TIMED_OUT as hda_get. */
status_t hda_set(struct hda *h, unsigned cad, unsigned nid, uint32_t verb, uint32_t payload);
/* SET_POWER_STATE D0 to node `nid`, then GET_POWER_STATE until the actual
 * state reads D0 (bounded: POWER_WAIT, logged). */
status_t hda_power_up(struct hda *h, unsigned cad, unsigned nid);

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
/* Read widget w (w->nid) of codec c again, from scratch, as
 * hda_read_codec does: what is set on it now. */
void     hda_read_widget(struct hda *h, const struct codec *c, struct widget *w);
/* The widget with node id nid, or NULL if c has none (or did not keep it). */
const struct widget *hda_widget(const struct codec *c, unsigned nid);

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

/* ---- the path to the headphones (path.c, pure: struct codec in, struct path out) ---- */

#define PATH_MAX_NODES 6   /* the pin and at most 5 widgets behind it */
#define PATH_MAX_ALSO  8

/* How the pin was chosen, best first (path.c's header has the rules). */
enum path_rule {
    PATH_NONE = 0, PATH_FRONT_HP, PATH_HP, PATH_LINE_OUT, PATH_SPEAKER,
};

struct path {
    uint8_t cad;                     /* the codec */
    uint8_t rule;                    /* enum path_rule; PATH_NONE: no path */
    uint8_t n;                       /* nodes on the path */
    uint8_t nid[PATH_MAX_NODES];     /* nid[0] the DAC ... nid[n - 1] the pin */
    uint8_t in[PATH_MAX_NODES];      /* i > 0: nid[i - 1]'s index in nid[i]'s connection list */
    bool    dac_shared;              /* another pin with its output on reaches the DAC */
    uint8_t nalso;                   /* other output-capable pins whose selected input is */
    uint8_t also[PATH_MAX_ALSO];     /* a node of the path: they would carry the same sound */
};

/* The path from an analog DAC to the best output pin of codec c.
 * ERR_NOT_FOUND (*out still written, rule PATH_NONE) if no output pin
 * reaches one. */
status_t hda_path_find(const struct codec *c, struct path *out);

/* ---- the path in words (dump.c) ----------------------------------------------------- */

/* The rule's words: "front headphone jack", "line-out", ... */
const char *hda_path_rule_name(unsigned rule);
/* "dac 02 -> mixer 0c -> pin 1b" into buf (always NUL-terminated). */
void hda_path_str(const struct codec *c, const struct path *p, char *buf, size_t size);
/* What is set on each node of the path, from c's widgets (read back after
 * programming): "afg D0; dac 02 D0 out m0; mixer 0c in m0 m0; pin 1b D0
 * ctl 20 (output off) out m0 eapd off". */
void hda_path_state(const struct codec *c, const struct path *p, char *buf, size_t size);
/* The path's line: "codec 0 path: dac 02 -> mixer 0c -> pin 1b (front
 * headphone jack); ...", or why there is none. */
void hda_dump_path(struct out *o, const struct codec *c, const struct path *p);

/* ---- programming it (verbs.c) --------------------------------------------------- */

/* Power up the audio function group and the path's widgets, turn the
 * pin's output off, mute every amplifier on the path (every input of a
 * mixer on it too) and select the path's inputs. No sound can come out
 * afterwards; the stream and the unmuting are not built yet. Stops at the
 * first verb that fails. */
status_t hda_path_program_muted(struct hda *h, const struct codec *c, const struct path *p);
/* Read the path's widgets and the AFG's power state back into c. */
void     hda_path_read_back(struct hda *h, struct codec *c, const struct path *p);

/* ---- fixtures (fixtures.c) --------------------------------------------------------- */

/* A codec rebuilt from its dump lines (dump.c's format; other lines are
 * skipped). ERR_INVALID_ARGS if a widget line does not parse. */
status_t hda_codec_from_dump(const char *text, size_t len, struct codec *out);
/* The path finder against every fixture (QEMU's codecs, the PC's): each
 * must give its known path. Uses *scratch; logs a line per failure and
 * one summary into o. true: all passed. */
bool     hda_path_selftest(struct codec *scratch, struct out *o);
/* Codec c's dump (read live) parsed back into *scratch must give the same
 * path p the live graph gave: the parser and dump.c agree, so a fixture
 * pasted from a log is the codec the driver saw. false: a line into o. */
bool     hda_path_roundtrip(const struct codec *c, const struct path *p, struct codec *scratch,
                            struct out *o);
