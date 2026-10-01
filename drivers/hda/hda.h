/* hda: the Intel High Definition Audio driver's own pieces (drv/hda).
 *
 * The driver resets the controller, finds the codecs on the link, prints
 * each codec's widget graph, picks the path from a DAC to the front-panel
 * headphone jack (path.c, checked at start against fixtures.c) and
 * programs it with every amplifier on it muted and the pin's output off;
 * then it serves abi/idl/hda.idl (the dump, the path, the gain and one
 * output stream on the path's DAC) until devmgr closes its channel. The
 * path is unmuted, at the gain, only while the stream runs. The jacks with
 * presence detection are watched (unsolicited responses, or polling) and
 * each change logged. Every verb
 * goes through verbs.c, which takes GET verbs and an allow-list of SET
 * verbs (routing, amplifiers, pin control, EAPD, power, converter format
 * and stream, unsolicited enable, pin sense) and refuses anything else,
 * so the board's own settings (configuration defaults, GPIOs, vendor
 * coefficients) are never written.
 *
 * Files: main.c (start, the protocol, exit), ctrl.c (the controller:
 * reset, the CORB/RIRB command rings, the immediate command interface,
 * stop), verbs.c (the verbs the driver may send), graph.c (reading a
 * codec's nodes into struct codec), dump.c (the readable lines), path.c
 * (the path finder), fixtures.c (its test codecs), stream.c (the output
 * stream), jack.c (the jacks), irq.c (the loop: the channels and the MSI).
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

/* Amplifier Capabilities (spec 7.3, parameters): gain steps 0..STEPS of
 * STEP_MDB thousandths of a dB each, 0 dB at step OFFSET; bit 31 mute. */
#define AMPCAP_OFFSET(c)   ((c) & 0x7fu)
#define AMPCAP_STEPS(c)    (((c) >> 8) & 0x7fu)
#define AMPCAP_STEP_MDB(c) (((((c) >> 16) & 0x7fu) + 1) * 250)
#define AMPCAP_MUTE        (1u << 31)

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
#define UNSOL_Q          32                  /* unsolicited responses held for jack.c */

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
    uint32_t unsol;            /* unsolicited responses seen */
    uint32_t unsol_dropped;    /* ... of them lost: the queue below was full */
    uint32_t late;             /* solicited responses no command was waiting for (a late
                                * answer to one that timed out, or a codec not asked) */
    uint32_t overruns;         /* RIRBSTS.OIS seen: the RIRB overflowed */
    bool     unsol_on;         /* GCTL.UNSOL and INTCTL.CIE are on */
    /* Unsolicited responses (their codec and the response word) in arrival
     * order, until jack.c takes them: a ring of UNSOL_Q from uq_head. */
    uint32_t uq_resp[UNSOL_Q];
    uint8_t  uq_cad[UNSOL_Q];
    uint8_t  uq_head, uq_n;
    uint32_t timeouts;         /* verbs that got no answer */
    uint16_t vendor, device;   /* the PCI ids */
    bool     dpib_ok;          /* HDA_BAR_MAP bytes are mapped: HDA_SD_DPIB can be read */
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

/* The RIRB's demultiplexer (ctrl.c). Every entry the controller writes
 * goes through hda_rirb_sort, whoever reads it (a command waiting for its
 * answer, or the RIRB interrupt): an unsolicited one (the entry's high
 * word, bit 4) is queued for jack.c and never taken for an answer; a
 * solicited one is the answer only if a command to its codec is waiting
 * (`want`, -1: none), else it is counted late and dropped. true: *out is
 * the answer. Pure but for h's counters and queue (the self-test feeds it
 * a fake RIRB). */
bool     hda_rirb_sort(struct hda *h, uint64_t entry, int want, uint32_t *out);
/* Every entry the controller has written since the last read, sorted with
 * want -1. Nothing without the rings. */
void     hda_rirb_drain(struct hda *h);
/* The RIRB interrupt (INTSTS.CIS): RIRBSTS read and cleared, then drained. */
void     hda_rirb_irq(struct hda *h);
/* The oldest queued unsolicited response: its codec and word. false: none. */
bool     hda_unsol_pop(struct hda *h, unsigned *cad, uint32_t *resp);
/* GCTL.UNSOL (the controller accepts unsolicited responses) and INTCTL's
 * CIE and GIE (the RIRB interrupt) on or off. On needs the rings. */
void     hda_unsol_enable(struct hda *h, bool on);

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
/* Centibels as dB with one decimal: "-30.0", "0.0", "-0.8". */
void hda_db_str(char *buf, size_t size, int32_t cb);
/* The path's line: "codec 0 path: dac 02 -> mixer 0c -> pin 1b (front
 * headphone jack); ...", or why there is none. */
void hda_dump_path(struct out *o, const struct codec *c, const struct path *p);

/* ---- programming it (verbs.c) --------------------------------------------------- */

/* Power up the audio function group and the path's widgets, turn the
 * pin's output off, mute every amplifier on the path (every input of a
 * mixer on it too) and select the path's inputs. No sound can come out
 * afterwards; hda_output_open unmutes it while a stream runs. Stops at the
 * first verb that fails. */
status_t hda_path_program_muted(struct hda *h, const struct codec *c, const struct path *p);
/* Read the path's widgets and the AFG's power state back into c. */
void     hda_path_read_back(struct hda *h, struct codec *c, const struct path *p);

/* ---- the output: the path opened while a stream plays (verbs.c) -------------------
 * main.c fills it once the path is set up muted; stream.c opens it just
 * before RUN and closes it right after RUN clears; main.c's set_gain
 * changes the gain. All on the driver's one thread. */

#define GAIN_DEFAULT_CB  (-300)   /* -30 dB: quiet in headphones (docs/A1-PLAN.md, stage 3) */

struct output {
    const struct codec *c;     /* the path's codec as read back after set-up; NULL: no path */
    const struct path  *p;
    uint8_t  vol;              /* the node whose output amp is the volume; 0: none has steps */
    uint32_t vol_amp;          /* its amplifier capabilities */
    uint8_t  step;             /* the volume amp's step to play at */
    bool     open;             /* unmuted now (hda_output_open succeeded, no close since) */
    bool     failed;           /* the last open failed (and was muted again) */
    uint32_t max_bits;         /* the largest sample size a stream may use (`hda bits`; 32) */
    bool     mutes;            /* an amp on the path can mute it: the amps are its mute and the
                                * output stage stays on (else the pin control is the mute) */
    bool     stage;            /* the output stage is on: the pin's output (and headphone amp)
                                * and EAPD */
    uint64_t stage_at;         /* when it went on (ns) */
};

/* How long the output stage (the pin's output driver and headphone amp,
 * and the amplifier EAPD powers) is left to settle, with every amp on the
 * path muted, before the first stream unmutes it. Switching it on charges
 * its output's DC-blocking capacitor: the thump the PC's headphones gave
 * at the boot splash's first sound, when that came within microseconds of
 * the stage going on. The spec gives no figure; Linux's Realtek code waits
 * 200 ms after turning EAPD off before the pins (alc_eapd_shutup's depop
 * delay), so twice that is taken here. It is paid once per driver start,
 * and only the part not yet gone by when the first stream opens. */
#define OUTPUT_SETTLE_NS (400 * NS_PER_MS)

/* o for path p of codec c (both NULL: no path), at GAIN_DEFAULT_CB, any
 * sample size. Sends nothing. */
void     hda_output_init(struct output *o, const struct codec *c, const struct path *p);
/* The output stage on (`on`: the pin's output, its headphone amp if it
 * has one, EAPD if the pin has it) or off (the reverse order), with the
 * path's amps left as they are. On a path whose amps can mute (o->mutes)
 * main.c turns it on once, right after the path is set up muted, and off
 * when the driver stops: streams then open and close the amps only, so
 * the stage's power-up thump is never heard. Each verb is tried; the first
 * failure is returned (on: the stage counts as off). ERR_NOT_FOUND: no
 * path. */
status_t hda_output_stage(struct hda *h, struct output *o, bool on);
/* The path opened (docs/A1-PLAN.md's steps 4-6). With o->mutes: the stage
 * on if it isn't (it should be), and OUTPUT_SETTLE_NS waited since it went
 * on, then the path's inputs on its mixers and selectors unmuted (the
 * others stay muted) and every output amp on it unmuted (the volume amp
 * at o->step, the rest at 0 dB). Without: the amps as above, then the
 * stage on, as the only mute there is. A verb that fails: logged, the path
 * closed again, its status returned. ERR_NOT_FOUND: no path. */
status_t hda_output_open(struct hda *h, struct output *o);
/* The reverse: every amp on the path muted at gain step 0, and without
 * o->mutes the stage off too. Every verb is tried even if one fails (the
 * first failure is returned). Does nothing if o is not open. */
status_t hda_output_close(struct hda *h, struct output *o);
/* The volume amp to the step nearest `cb` centibels, clamped to its range
 * and to 0 dB; sent at once if o is open. ERR_NOT_FOUND: no path;
 * ERR_NOT_SUPPORTED: no amp with steps. */
status_t hda_output_set_gain(struct hda *h, struct output *o, int32_t cb);
/* The gain now in centibels, and the range the volume amp allows (to 0
 * dB). ERR_NOT_FOUND / ERR_NOT_SUPPORTED as hda_output_set_gain. */
status_t hda_output_gain(const struct output *o, int32_t *cb, int32_t *min, int32_t *max);

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

/* ---- the jacks (jack.c) ---------------------------------------------------------------
 * Every pin whose configuration default says it is a jack and whose pin
 * capabilities have presence detection, each with its own unsolicited
 * response tag; its presence (GET_PIN_SENSE bit 31) debounced and logged
 * when it changes. Spec 7.3 (Unsolicited Response, Pin Sense), 7.3.4.9 (pin
 * capabilities: trigger, presence detect). */

#define MAX_JACKS        16
#define JACK_POLL_NS     (500 * NS_PER_MS)   /* a polled jack's presence is read this often */
#define JACK_DEBOUNCE_NS (80 * NS_PER_MS)    /* a new presence must hold this long */
#define JACK_SETTLE_NS   NS_PER_MS           /* after SET_PIN_SENSE, before GET_PIN_SENSE */
#define UNSOL_TAG(resp)  ((resp) >> 26)      /* an unsolicited response: 31:26 the tag */

enum jack_state { JACK_UNKNOWN = 0, JACK_OUT = 1, JACK_IN = 2 };   /* hda.idl's numbers */

/* How a jack's changes are found. */
enum jack_mode {
    JM_POLL = 0,    /* polled: no unsolicited responses (the pin, or the controller's
                     * interrupt, can't) */
    JM_TRY,         /* unsolicited responses on, and polled until one proves they work */
    JM_UNSOL,       /* proven: unsolicited responses only, not polled */
    JM_MISSED,      /* a change came with no unsolicited response: polled for good */
};

struct jack {
    uint8_t  cad, nid, tag;      /* the codec, the pin, its tag (1..MAX_JACKS) */
    uint32_t config, pincaps;    /* the pin's configuration default and capabilities */
    bool     can_unsol;          /* the pin's widget capabilities have Unsol Capable */
    uint8_t  mode;               /* enum jack_mode */
    uint8_t  state;              /* enum jack_state: the debounced presence */
    bool     pending;            /* a read differed from state, at pending_at */
    uint64_t pending_at;
    bool     heard;              /* an unsolicited response for it since the last change */
    uint32_t changes;            /* state changes after the first read */
    uint32_t unsols;             /* unsolicited responses with its tag */
    uint32_t errors;             /* presence reads that failed */
};

struct jacks {
    unsigned n;
    struct jack j[MAX_JACKS];
    bool     unsol;              /* unsolicited responses were turned on at start */
    uint32_t irq_seen;           /* RIRB interrupts since then (irq.c counts them) */
    uint32_t stray;              /* unsolicited responses with no jack's tag */
    uint64_t next_poll;
};

/* What jack.c sends and hears, through: hda_set/hda_get and the unsolicited
 * queue for the driver (hda_jack_io), a fake codec for the self-test. */
struct jack_io {
    void    *ctx;
    status_t (*set)(void *ctx, unsigned cad, unsigned nid, uint32_t verb, uint32_t payload);
    status_t (*get)(void *ctx, unsigned cad, unsigned nid, uint32_t verb, uint32_t payload,
                    uint32_t *out);
    bool     (*unsol)(void *ctx, unsigned *cad, uint32_t *resp);   /* the next one queued */
    void     (*sleep)(void *ctx, uint64_t ns);
    void     (*say)(void *ctx, const char *line);                  /* a log line */
};

/* The driver's io on controller h. */
void     hda_jack_io(struct jack_io *io, struct hda *h);
/* Codec c's jack pins appended to js (pure); tags in order from 1. */
void     hda_jacks_add(struct jacks *js, const struct codec *c);
/* The jack an unsolicited response `resp` from codec cad is for, or NULL. */
struct jack *hda_jack_for(struct jacks *js, unsigned cad, uint32_t resp);
/* "headphones", and "front" (with the colour if another jack of the same
 * kind is there too: "rear green"). */
const char *hda_jack_name(const struct jack *j);
void     hda_jack_where(const struct jacks *js, const struct jack *j, char *buf, size_t size);
/* How a jack's changes are found, in words (enum jack_mode). */
const char *hda_jack_mode_str(unsigned mode);
/* Unsolicited responses on for each jack pin that can send them (if
 * `unsol`: the controller takes them and its interrupt works), every jack's
 * presence read, one line each logged. */
void     hda_jacks_start(struct jacks *js, const struct jack_io *io, bool unsol, uint64_t now);
/* The queued unsolicited responses, the debounce reads due and the poll
 * (if due): each change logged. */
void     hda_jacks_run(struct jacks *js, const struct jack_io *io, uint64_t now);
/* When hda_jacks_run next has something to do (DEADLINE_NEVER: only an
 * unsolicited response can give it any). */
uint64_t hda_jacks_deadline(const struct jacks *js);
/* Unsolicited responses off on every pin they were turned on for. */
void     hda_jacks_stop(struct jacks *js, const struct jack_io *io);
/* The jack logic against fixtures and a fake codec (jack.c). */
bool     hda_jack_selftest(struct codec *scratch, struct out *o);
/* Fixture `name` (fixtures.c's table) parsed into *out, its variation
 * applied. ERR_NOT_FOUND / the parser's status. */
status_t hda_fixture(const char *name, struct codec *out);

/* ---- the output stream (stream.c) and the driver's loop (irq.c) -------------------
 * One output stream: the first output stream descriptor (index ISS, as
 * GCAP counts them) feeding the DAC of the path main.c chose, stream tag
 * 1, 48 kHz stereo at a sample size the DAC takes (16, 20, 24 or 32 bits:
 * the client asks; 20 and 24 travel in 32-bit containers, left-justified),
 * from a DMA32 ring of RING_FRAMES frames in PERIODS periods. Its Buffer
 * Descriptor List and the DMA position buffer share one more DMA32 page.
 * Spec chapter 3 (stream descriptor registers, DPLBASE) and chapter 4
 * (stream setup, 4.5.1 the samples in memory). */

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

#define STREAM_TAG     1u
#define STREAM_RATE    48000u
#define FMT_48K_STEREO 0x0001u     /* the stream format: 48 kHz (base 48, x1, /1), 2 channels */
#define FMT_BITS(b)    ((b) << 4)  /* bits 6:4: 0 8-bit, 1 16, 2 20, 3 24, 4 32 */
#define PCM_SIZE(bits) ((bits) == 16 ? 1u << 17 : (bits) == 20 ? 1u << 18 : \
                        (bits) == 24 ? 1u << 19 : (bits) == 32 ? 1u << 20 : 0)   /* P_PCM 20:16 */
#define RING_FRAMES    16384u      /* 341 ms */
#define PERIODS        8u
#define PERIOD_FRAMES  (RING_FRAMES / PERIODS)   /* 2048: 42.7 ms */
#define RING_BYTES_MAX (RING_FRAMES * 8u)        /* 32-bit containers */
#define PERIOD_NS      ((uint64_t)PERIOD_FRAMES * NS_PER_S / STREAM_RATE)
#define STALL_PERIODS  8u          /* no progress this long while running (341 ms): stalled */
/* Intel's vendor register beside the spec's: the DMA position in buffer
 * of stream descriptor n (SDxDPIB, Intel PCH datasheets; on Skylake and
 * later PCHs the position of the DMA engine itself). Read only for the
 * log: where the controller's DMA is compared to the position buffer. */
#define HDA_SD_DPIB(n) (0x1084u + 0x20u * (n))
#define HDA_BAR_MAP    0x2000u     /* the BAR bytes mapped: the spec's registers and DPIB */

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
    unsigned cad, dac;         /* the output converter the stream feeds (the path's DAC) */
    bool     has_dac;          /* there is one: main.c found and set up a path */
    struct output *out;        /* the path, opened while the stream runs */
    struct dma_buf ring;       /* the samples, shared with the client */
    struct dma_buf page;       /* the BDL at 0, the DMA position buffer at POS_OFF */
    /* the format of the open stream: its sample size, the format word, a
     * frame's bytes (4 or 8), the ring's and a period's bytes */
    uint32_t bits;
    uint16_t fmt;
    uint32_t frame_bytes, ring_bytes, period_bytes;
    /* the position: the byte offset read last, bytes played since the
     * open, bytes zeroed behind the play position (the same after every
     * update), and when `played` last grew */
    uint32_t last_off;
    uint64_t played, cleared;
    uint64_t progress_ns;
    uint32_t iocs;             /* buffer-completion interrupts taken */
    uint32_t fifo_errors;      /* FIFOE/DESE seen */
    uint32_t lpib_diff_max;    /* largest gap between the position buffer and LPIB, bytes */
    uint32_t dpib_diff_max;    /* ... and Intel's DPIB (bytes; dpib_seen: it ever read non-0) */
    bool     dpib_seen;
};

/* stream.c. Pick the output stream descriptor (none if GCAP has no
 * output streams) and take the DAC from out's path (none: every open
 * fails ERR_NOT_FOUND); touches no register. */
void     stream_init(struct hda *h, struct stream *s, handle_t dev, struct output *out);
/* Open it (the checks and results of hda.idl's open_output) at `bits`
 * per sample (16, 20, 24 or 32; ERR_NOT_SUPPORTED if the DAC does not
 * take it, or it is above out->max_bits): DMA buffers, stream reset and
 * setup, the converter's format and stream tag, the stream's interrupt
 * enabled. *ring: the client's handle. */
status_t stream_open(struct hda *h, struct stream *s, uint32_t bits, handle_t *ring);
/* The DAC's PCM sizes and rates (P_PCM) as open_output may use them: the
 * sizes above out->max_bits taken out (16-bit always stays). 0: no DAC. */
uint32_t hda_output_pcm(const struct output *o);
status_t stream_start(struct hda *h, struct stream *s);
status_t stream_stop(struct hda *h, struct stream *s);
/* Read the position, count what played and zero the ring behind it. */
void     stream_update(struct hda *h, struct stream *s);
/* The stream's status bits, read and cleared (from the interrupt). */
void     stream_status(struct hda *h, struct stream *s);
/* Stop the DMA engine (RUN clear, waited for), reset the stream, point
 * the converter at no stream, release the buffers. Safe when not open. */
void     stream_close(struct hda *h, struct stream *s, const char *why);

/* Wait (bounded, logged on a timeout) until (8-bit register reg & mask)
 * == want. ERR_TIMED_OUT. */
status_t hda_wait8(struct hda *h, uint32_t reg, uint8_t mask, uint8_t want, const char *what);

/* irq.c. Serve `ops` (ctx) on DR_SERVE and on the query channels
 * hda.query hands out (there without open_output and query), the output
 * stream (on out's path) on the channels open_output hands out, the
 * controller's MSI and
 * the jacks js (started here: unsolicited responses on if the rings and
 * the MSI are there), until devmgr closes DR_SERVE (OK) or a wait fails
 * (its status); the stream is closed (and the path muted) and unsolicited
 * responses turned off on the way out. */
struct hda_ops;
status_t hda_loop(struct hda *h, const struct driver_start *ds, const struct hda_ops *ops,
                  void *ctx, struct output *out, struct jacks *js);
