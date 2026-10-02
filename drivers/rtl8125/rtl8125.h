/* rtl8125: the listen-only probe's own pieces (drv/rtl8125).
 *
 * The probe learns what the rest of the network milestone needs to know
 * about the PC's Realtek RTL8125B and the switch port behind it, and
 * sends nothing (main.c has the steps and the list of every register it
 * writes; notx.h the rule that refuses a transmit register).
 *
 * Files: main.c (the steps, the summary and the RESULTS line), regs.c
 * (register access through the guard, the chip's OCP windows to the MAC
 * and the PHY, waits, the tally counter dump), chip.c (the firmware's
 * state, the chip's identity, reset, receive-only bring-up,
 * autonegotiation, link, stop), census.c (the receive ring and the count
 * of frames by tag).
 *
 * Registers, bits and the order of bring-up follow OpenBSD's rge(4)
 * driver (sys/dev/pci/if_rge.c and if_rgereg.h, by Kevin Lo, ISC
 * licence), cited by its function names; the MII registers (0-15) are
 * IEEE 802.3 clause 22. Values written are rge's for the 8125B ("MAC_R25B")
 * unless a comment says otherwise. Linux's r8169 (GPL) is not used. */
#pragma once

#include <jam/driver.h>
#include <jam/netframe.h>
#include "notx.h"

/* ---- registers (rge: if_rgereg.h) --------------------------------------------- */

#define RTL_MAC0        0x00     /* 8 x 6: the station address */
#define RTL_MAR0        0x08     /* 32: multicast filter, hashes 32-63 */
#define RTL_MAR4        0x0c     /* 32: hashes 0-31 */
#define RTL_DTCCR_LO    0x10     /* 32: tally counters dump address, low (bit 3: dump now) */
#define RTL_DTCCR_HI    0x14
#define RTL_DTCCR_CMD   0x08
#define RTL_INT_CFG0    0x34     /* 8: interrupt configuration */
#define RTL_INT_CFG0_EN 0x01
#define RTL_CMD_RXBUF_EMPTY 0x01
#define RTL_CMD_RXENB   0x08
#define RTL_CMD_RESET   0x10
#define RTL_CMD_STOPREQ 0x80
#define RTL_IMR         0x38     /* 32: interrupt mask */
#define RTL_ISR         0x3c     /* 32: interrupt status (write 1 to clear) */
#define RTL_RXCFG       0x44     /* 32: receive configuration */
#define RTL_EECMD       0x50     /* 8: config register unlock */
#define RTL_CFG1        0x52
#define RTL_CFG2        0x53
#define RTL_CFG3        0x54
#define RTL_CFG5        0x56
#define RTL_TIMERINT(n) ((n) == 3 ? 0xf4 : (n) == 2 ? 0x8c : 0x58 + 4 * (n))   /* 32 */
#define RTL_PHYSTAT     0x6c     /* 16: the PHY's resolved link */
#define RTL_PMCH        0x6f     /* 8: power management */
#define RTL_DLLPR       0xd0     /* 8 */
#define RTL_TWICMD      0xd2     /* 16 */
#define RTL_MCUCMD      0xd3     /* 8: the chip's own MCU */
#define RTL_RXMAXSIZE   0xda     /* 16: longest frame accepted, bytes */
#define RTL_CPLUSCMD    0xe0     /* 16 */
#define RTL_IM          0xe2     /* 16: hardware interrupt moderation */
#define RTL_RXDESC_LO   0xe4     /* 32: the receive ring's address, low */
#define RTL_RXDESC_HI   0xe8
#define RTL_PPSW        0xf2     /* 8 */
#define RTL_MACOCP      0xb0     /* 32: the window to the MAC's OCP registers */
#define RTL_PHYOCP      0xb8     /* 32: the window to the PHY's OCP registers */
#define RTL_INTMITI(i)  (0x0a00 + (i) * 4)
#define RTL_ADDR0       0x19e0   /* 32 + 16: the chip's own copy of its address */
#define RTL_ADDR1       0x19e4
#define RTL_RSS_CTRL    0x4500   /* 8 */
#define RTL_RXQUEUE_CTRL 0x4800  /* 16 */

/* RXCFG bits */
#define RTL_RXCFG_ALLPHYS   0x00000001u   /* every unicast address */
#define RTL_RXCFG_INDIV     0x00000002u   /* our own address */
#define RTL_RXCFG_MULTI     0x00000004u
#define RTL_RXCFG_BROAD     0x00000008u
#define RTL_RXCFG_RUNT      0x00000010u
#define RTL_RXCFG_ERRPKT    0x00000020u
#define RTL_RXCFG_VLANSTRIP 0x00c00000u   /* the chip's tag stripping: kept OFF */
#define RTL_RXCFG_ACCEPT    (RTL_RXCFG_ALLPHYS | RTL_RXCFG_INDIV | RTL_RXCFG_MULTI | \
                             RTL_RXCFG_BROAD)
#define RTL_RXCFG_8125B     0x41000c00u   /* rge's RGE_RXCFG_CONFIG_8125B */

/* ISR / IMR bits */
#define RTL_ISR_RX_OK       0x0001u
#define RTL_ISR_RX_ERR      0x0002u
#define RTL_ISR_TX_OK       0x0004u
#define RTL_ISR_TX_ERR      0x0008u
#define RTL_ISR_RX_DESC_UNAVAIL 0x0010u
#define RTL_ISR_LINKCHG     0x0020u
#define RTL_ISR_RX_FIFO_OFLOW 0x0040u
#define RTL_ISR_TX_DESC_UNAVAIL 0x0080u
#define RTL_ISR_PCS_TIMEOUT 0x4000u
#define RTL_ISR_SYSTEM_ERR  0x8000u
#define RTL_ISR_TX_ANY      (RTL_ISR_TX_OK | RTL_ISR_TX_ERR | RTL_ISR_TX_DESC_UNAVAIL)
/* What the probe unmasks: receive and link only, nothing of the transmitter. */
#define RTL_IMR_PROBE       (RTL_ISR_RX_OK | RTL_ISR_RX_ERR | RTL_ISR_RX_DESC_UNAVAIL | \
                             RTL_ISR_LINKCHG | RTL_ISR_RX_FIFO_OFLOW | RTL_ISR_PCS_TIMEOUT | \
                             RTL_ISR_SYSTEM_ERR)

/* PHYSTAT bits */
#define RTL_PHYSTAT_FDX     0x0001u
#define RTL_PHYSTAT_LINK    0x0002u
#define RTL_PHYSTAT_10      0x0004u
#define RTL_PHYSTAT_100     0x0008u
#define RTL_PHYSTAT_1000    0x0010u
#define RTL_PHYSTAT_RXFLOW  0x0020u   /* pause frames honoured (flow control resolved on) */
#define RTL_PHYSTAT_TXFLOW  0x0040u   /* pause frames sent: must stay 0 */
#define RTL_PHYSTAT_2500    0x0400u

#define RTL_MCUCMD_RXFIFO_EMPTY 0x10
#define RTL_MCUCMD_TXFIFO_EMPTY 0x20
#define RTL_MCUCMD_IS_OOB   0x80      /* the chip's firmware owns it ("out of band") */
#define RTL_EECMD_WRITECFG  0xc0
#define RTL_CFG1_SPEED_DOWN 0x10
#define RTL_CFG3_WOL_LINK   0x10
#define RTL_CFG3_WOL_MAGIC  0x20
#define RTL_CFG5_WOL_ANY    0x72      /* LANWAKE, unicast, multicast, broadcast wake */

/* The OCP windows (rge_write_mac_ocp, rge_read_phy_ocp, ...) */
#define RTL_OCP_BUSY        0x80000000u
#define RTL_OCP_ADDR_SHIFT  16

/* MII registers (802.3 clause 22), reached through the PHY's OCP space */
#define MII_BMCR        0
#define MII_BMSR        1
#define MII_PHYID1      2
#define MII_PHYID2      3
#define MII_ANAR        4
#define MII_GTCR        9
#define BMCR_RESET      0x8000u
#define BMCR_AUTOEN     0x1000u
#define BMCR_STARTNEG   0x0200u
#define ANAR_10         0x0020u
#define ANAR_10_FD      0x0040u
#define ANAR_TX         0x0080u
#define ANAR_TX_FD      0x0100u
#define ANAR_PAUSE      0x0400u   /* symmetric pause: never advertised */
#define ANAR_PAUSE_ASYM 0x0800u   /* asymmetric pause: never advertised */
#define GTCR_1000_HDX   0x0100u
#define GTCR_1000_FDX   0x0200u

/* PHY OCP registers (rge) */
#define PHY_ADV_2500    0xa5d4    /* bit 7: advertise 2500BASE-T */
#define PHY_ADV_2500_FD 0x0080u
#define PHY_STATE       0xa420    /* bits 2:0: the PHY's state (3: ready) */
#define PHY_RAM_INDEX   0xa436    /* index register into the PHY's parameter RAM */
#define PHY_RAM_DATA    0xa438
#define PHY_RAM_RCODE   0x801e    /* the PHY patch ("ram code") version */
#define RCODE_8125B     0x0b99    /* the version rge's 8125B patch writes (RGE_MAC_R25B_RCODE_VER) */

/* ---- the receive ring ------------------------------------------------------------ */

/* rge's receive descriptor on the 8125 is 32 bytes (struct rge_rx_desc,
 * with RXCFG 0x41000c00): the buffer's address at 16, extended status at
 * 24, command and status at 28. */
#define RX_DESCS        256
#define RX_DESC_SIZE    32
#define RX_BUF          2048      /* bytes per buffer: two per page */
#define RX_DESC_ADDR    16
#define RX_DESC_EXTSTS  24
#define RX_DESC_CMDSTS  28
#define RX_OWN          0x80000000u   /* the chip's until it has filled it */
#define RX_EOR          0x40000000u   /* the last descriptor: the ring wraps after it */
#define RX_SOF          0x02000000u
#define RX_EOF          0x01000000u
#define RX_ERRSUM       0x00100000u
#define RX_LEN          0x00003fffu   /* bytes received, the CRC included */
#define RING_BYTES      (RX_DESCS * RX_DESC_SIZE)   /* 8 KiB */
#define TALLY_OFF       RING_BYTES                  /* the tally dump after it (64-aligned) */
#define RING_VMO        (4 * 4096)
#define BUF_BYTES       (RX_DESCS * RX_BUF)
#define BUF_PAGES       (BUF_BYTES / 4096)

/* ---- state ----------------------------------------------------------------------- */

#define CENSUS_GROUPS   16        /* distinct tags counted (untagged, priority, VLANs, ...) */
#define CENSUS_TYPES    6         /* EtherTypes kept per group; the rest are "other" */
#define PROBE_VLAN      21        /* the VLAN the verdict looks for (ARCHITECTURE "Networking") */

/* One kind of frame by its tag, and the EtherTypes seen in it. */
struct group {
    uint8_t  kind;                /* enum netframe_kind */
    uint16_t tpid, vid;           /* the tag (0, 0 untagged) */
    uint32_t frames;
    uint16_t type[CENSUS_TYPES];  /* EtherTypes in order of first sighting */
    uint32_t type_n[CENSUS_TYPES];
    uint32_t other_types;         /* frames of EtherTypes past the first CENSUS_TYPES */
};

/* The 60 s count; nothing in it is an address or a payload byte. */
struct census {
    struct group g[CENSUS_GROUPS];
    unsigned ng;
    uint32_t lost;                /* frames of tags past the first CENSUS_GROUPS */
    uint32_t frames;              /* good, whole frames counted */
    uint32_t runts, errors, split;   /* too short; the chip's error bit; not in one buffer */
    uint32_t by_irq, by_poll;     /* frames found after an interrupt / at a 1 s poll */
    uint32_t irqs, polls;         /* interrupt packets (fires coalesced) / 1 s timeouts */
    uint32_t isr_seen;            /* every ISR bit seen */
    uint32_t link_changes;
};

struct rtl {
    volatile void *r;             /* BAR 2: the registers (64 KiB) */
    handle_t dev, dma, irq, serve, port;
    uint32_t xid;                 /* TXCFG bits 30:26, 23:20 (0x641: 8125B) */
    uint32_t refused;             /* writes rtl_write_allowed refused (must stay 0) */
    uint32_t ocp_timeouts;        /* OCP and CSI waits that ran out */
    /* the receive ring, its buffers and the tally dump */
    handle_t ring_vmo, buf_vmo;
    uint8_t *ring, *bufs;         /* mapped */
    uint64_t ring_pin, buf_pin;
    bool     ring_pinned, buf_pinned, bus_master, rx_on;
    uint64_t ring_addr;           /* device addresses */
    uint64_t buf_addr[BUF_PAGES];
    unsigned next;                /* the next descriptor to look at */
    /* the link */
    bool     link;
    uint16_t phystat;             /* the last PHYSTAT read */
    uint64_t link_at;             /* when it came up (uptime, ns), 0: not yet */
    unsigned link_lines;          /* link-change lines logged (capped) */
    struct census c;
};

/* The 8125's tally counters as it dumps them (rge's struct rge_stats). */
struct tally {
    uint64_t tx_ok, rx_ok, tx_err;
    uint32_t rx_err;
    uint16_t miss, fae;
    uint32_t tx_1col, tx_mcol;
    uint64_t rx_ok_phy, rx_ok_brd;
    uint32_t rx_ok_mul;
    uint16_t tx_abort, tx_underrun;
};

/* ---- regs.c ---------------------------------------------------------------------- */

uint8_t  rd8(const struct rtl *t, uint32_t reg);
uint16_t rd16(const struct rtl *t, uint32_t reg);
uint32_t rd32(const struct rtl *t, uint32_t reg);
/* Writes go through rtl_write_allowed: a refused one is dropped, counted
 * and logged. */
void     wr8(struct rtl *t, uint32_t reg, uint8_t v);
void     wr16(struct rtl *t, uint32_t reg, uint16_t v);
void     wr32(struct rtl *t, uint32_t reg, uint32_t v);
void     set8(struct rtl *t, uint32_t reg, uint8_t bits);
void     clr8(struct rtl *t, uint32_t reg, uint8_t bits);
uint16_t mac_rd(struct rtl *t, uint16_t reg);
void     mac_wr(struct rtl *t, uint16_t reg, uint16_t v);
void     mac_mod(struct rtl *t, uint16_t reg, uint16_t clear, uint16_t set);
uint16_t phy_rd(struct rtl *t, uint16_t reg);
void     phy_wr(struct rtl *t, uint16_t reg, uint16_t v);
void     phy_mod(struct rtl *t, uint16_t reg, uint16_t clear, uint16_t set);
uint16_t mii_rd(struct rtl *t, unsigned reg);
void     mii_wr(struct rtl *t, unsigned reg, uint16_t v);
/* Busy-wait (short) or sleep (from 1 ms) for us microseconds. */
void     delay_us(uint64_t us);
/* The tally counters into *out (needs the receiver on and bus mastering). */
status_t tally_dump(struct rtl *t, struct tally *out);

/* ---- chip.c ---------------------------------------------------------------------- */

void     chip_snapshot(struct rtl *t);
/* The id from TXCFG; ERR_NOT_SUPPORTED (logged) for anything but the 8125B. */
status_t chip_identify(struct rtl *t);
void     chip_log_mac(struct rtl *t);
/* rge_stop + rge_exit_oob + PHY power: the chip quiet, owned by us. */
status_t chip_reset(struct rtl *t);
/* After reset: the PHY's id and patch version, the MAC MCU, the receive
 * registers' values. Returns the PHY's id (PHYID1 << 16 | PHYID2). */
uint32_t chip_log_after_reset(struct rtl *t, uint16_t *rcode);
/* The receive path on the ring at t->ring_addr, accept-all, tags kept. */
void     chip_rx_start(struct rtl *t);
void     chip_autoneg(struct rtl *t);
/* PHYSTAT read; logs a change (capped). True if the link is up. */
bool     chip_link_poll(struct rtl *t, uint64_t since);
void     chip_link_str(uint16_t phystat, char *buf, size_t size);
/* The receiver off and the chip reset; no DMA after it returns. */
void     chip_stop(struct rtl *t);

/* ---- census.c -------------------------------------------------------------------- */

/* The ring and buffers: made, pinned (bus mastering must be on), armed. */
status_t ring_setup(struct rtl *t);
void     ring_free(struct rtl *t);
/* Every frame the chip has handed back, counted; returns how many. */
unsigned census_harvest(struct rtl *t, bool by_irq);
/* One frame's tag into the count (pure: for the tests' sake too). */
void     census_count(struct census *c, const struct netframe_class *f);
void     census_log(const struct census *c);
/* The verdict from the plan's rules, "trunk carrying 21", ... */
const char *census_verdict(const struct census *c);
/* Frames on VLAN v (tagged 802.1Q), untagged, every tagged one. */
uint32_t census_vlan(const struct census *c, uint16_t v);
uint32_t census_kind(const struct census *c, enum netframe_kind k);
