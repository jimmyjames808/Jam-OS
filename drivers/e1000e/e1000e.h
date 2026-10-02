/* e1000e: the driver for QEMU's emulated Intel 82574L (drv/e1000e, PCI
 * 8086:10d3), the network card every network test in QEMU uses: QEMU
 * can't emulate the PC's RTL8125, so this small driver speaks the same
 * netdev protocol (abi/idl/netdev.idl, <jam/netdev.h>) and the same frame
 * rules (<jam/netframe.h>), and everything above the driver is tested in
 * QEMU (docs/M9-PLAN.md "Testing without the real NIC").
 *
 * Files: main.c (start, the list of every register written, stop),
 * chip.c (reset, the address, the PHY, flow control and VLAN offloads
 * off, link, the chip's counters), ring.c (the descriptor rings and
 * buffers in the driver's own DMA memory), rx.c (received frames: keep
 * our VLAN, untag, to the netdev server), tx.c (THE transmit path: copy,
 * tag, check, doorbell), loop.c (the one port everything arrives on, and
 * the card as the netdev server sees it). The server itself (info, stats,
 * open, the session and its rings) is the one every network driver
 * links, drivers/lib/netserver.c (<jam/netserver.h>).
 *
 * The VLAN rule (ARCHITECTURE.md "Networking"): every frame sent is
 * tagged 802.1Q with the VLAN devmgr passed (`vlan=<id>`), in software in
 * the driver's own buffer (netframe_tag), and checked once more right
 * before its descriptor is handed over (netframe_tx_check). The chip's own
 * tag insertion and stripping (CTRL.VME, the descriptors' VLE bit) and its
 * VLAN filter (RCTL.VFE) stay off, so tags pass as bytes both ways. With
 * no VLAN the chip is never touched. Flow control is off: the chip never
 * sends a PAUSE frame of its own.
 *
 * Registers and descriptors: the Intel 82574 GbE Controller Family
 * datasheet (section numbers below), and FreeBSD's em(4) driver (sys/dev/
 * e1000: e1000_regs.h, e1000_defines.h, if_em.c; BSD-2-Clause) for names
 * and the init order, cited by function. QEMU's hw/net/e1000e_core.c was
 * read for how the emulation behaves. Linux's e1000e (GPL) is not used. */
#pragma once

#include <jam/driver.h>
#include <jam/netdev.h>
#include <jam/netframe.h>
#include <jam/netserver.h>

/* ---- registers (82574 datasheet 10.2; em's e1000_regs.h names) --------------------- */

#define E1K_REGS_BYTES 0x20000   /* BAR 0: 128 KiB of registers */

#define E1K_CTRL      0x00000    /* device control */
#define E1K_STATUS    0x00008    /* device status */
#define E1K_EECD      0x00010    /* EEPROM control */
#define E1K_EERD      0x00014    /* EEPROM read */
#define E1K_CTRL_EXT  0x00018    /* extended device control */
#define E1K_MDIC      0x00020    /* MDI control: the PHY's registers */
#define E1K_FCAL      0x00028    /* flow control address, low */
#define E1K_FCAH      0x0002c
#define E1K_FCT       0x00030    /* flow control type */
#define E1K_VET       0x00038    /* VLAN EtherType (used only with CTRL.VME: off) */
#define E1K_ICR       0x000c0    /* interrupt cause (write 1 to clear) */
#define E1K_ITR       0x000c4    /* interrupt throttling */
#define E1K_IMS       0x000d0    /* interrupt mask set */
#define E1K_IMC       0x000d8    /* interrupt mask clear */
#define E1K_EIAC      0x000dc    /* MSI-X auto-clear */
#define E1K_IAM       0x000e0    /* interrupt auto-mask */
#define E1K_IVAR      0x000e4    /* MSI-X: which vector each cause uses (82574 only) */
#define E1K_RCTL      0x00100    /* receive control */
#define E1K_FCTTV     0x00170    /* flow control transmit timer */
#define E1K_TCTL      0x00400    /* transmit control */
#define E1K_TIPG      0x00410    /* transmit inter-packet gap */
#define E1K_FCRTL     0x02160    /* flow control receive threshold, low */
#define E1K_FCRTH     0x02168    /* ... high */
#define E1K_RDBAL     0x02800    /* receive ring 0: base, length, head, tail */
#define E1K_RDBAH     0x02804
#define E1K_RDLEN     0x02808
#define E1K_RDH       0x02810
#define E1K_RDT       0x02818
#define E1K_RDTR      0x02820    /* receive delay timer */
#define E1K_RADV      0x0282c    /* receive absolute delay */
#define E1K_TDBAL     0x03800    /* transmit ring 0: base, length, head, tail */
#define E1K_TDBAH     0x03804
#define E1K_TDLEN     0x03808
#define E1K_TDH       0x03810
#define E1K_TDT       0x03818
#define E1K_TIDV      0x03820    /* transmit interrupt delay */
#define E1K_TXDCTL    0x03828    /* transmit descriptor control */
#define E1K_TADV      0x0382c    /* transmit absolute delay */
#define E1K_RXCSUM    0x05000    /* receive checksum offload: off */
#define E1K_RFCTL     0x05008    /* receive filter control: 0, legacy descriptors */
#define E1K_MTA       0x05200    /* multicast table, 128 words */
#define E1K_RAL(n)    (0x05400 + 8 * (n))   /* receive address n, low */
#define E1K_RAH(n)    (0x05404 + 8 * (n))   /* ... high, with the valid bit */
#define E1K_VFTA      0x05600    /* VLAN filter table, 128 words: unused (RCTL.VFE off) */
#define E1K_MTA_WORDS 128
#define E1K_RAR_COUNT 16         /* receive address registers */

/* The chip's statistics (datasheet 10.2.6): 32 bits, cleared on read. */
#define E1K_CRCERRS   0x04000
#define E1K_ALGNERRC  0x04004
#define E1K_RXERRC    0x0400c
#define E1K_MPC       0x04010    /* missed: no receive descriptor */
#define E1K_ECOL      0x04018    /* excessive collisions */
#define E1K_LATECOL   0x04020
#define E1K_XONTXC    0x0404c    /* XON (pause) frames sent: must stay 0 */
#define E1K_XOFFTXC   0x04054    /* XOFF frames sent: must stay 0 */
#define E1K_GPRC      0x04074    /* good frames received */
#define E1K_GPTC      0x04080    /* good frames sent */

/* CTRL bits (10.2.2.1) */
#define CTRL_FD          (1u << 0)
#define CTRL_GIO_MDIS    (1u << 2)    /* GIO master disable: no new DMA */
#define CTRL_ASDE        (1u << 5)    /* auto-speed detection */
#define CTRL_SLU         (1u << 6)    /* set link up */
#define CTRL_FRCSPD      (1u << 11)
#define CTRL_FRCDPLX     (1u << 12)
#define CTRL_RST         (1u << 26)   /* device reset, self-clearing */
#define CTRL_RFCE        (1u << 27)   /* honour received PAUSE frames: off */
#define CTRL_TFCE        (1u << 28)   /* send PAUSE frames: OFF */
#define CTRL_VME         (1u << 30)   /* hardware VLAN tag insertion/stripping: OFF */
#define CTRL_PHY_RST     (1u << 31)

/* STATUS bits (10.2.2.2) */
#define STATUS_FD        (1u << 0)
#define STATUS_LU        (1u << 1)
#define STATUS_SPEED(s)  (((s) >> 6) & 3u)   /* 0: 10, 1: 100, 2 or 3: 1000 Mb/s */
#define STATUS_GIO_MEN   (1u << 19)   /* GIO master enable: DMA may still run */

#define EECD_AUTO_RD     (1u << 9)    /* the EEPROM's contents loaded after reset */
#define EERD_START       (1u << 0)
#define EERD_DONE        (1u << 1)
#define EERD_ADDR_SHIFT  2
#define EERD_DATA_SHIFT  16

#define CTRL_EXT_DRV_LOAD (1u << 28)  /* the driver has the chip (em_get_hw_control) */
#define CTRL_EXT_PBA_CLR  (1u << 31)  /* MSI-X pending bits cleared on read (em's 82574 init) */

/* MDIC (10.2.2.7): one PHY register access */
#define MDIC_DATA_MASK   0xffffu
#define MDIC_REG_SHIFT   16
#define MDIC_PHY_SHIFT   21
#define MDIC_OP_WRITE    (1u << 26)
#define MDIC_OP_READ     (2u << 26)
#define MDIC_READY       (1u << 28)
#define MDIC_ERROR       (1u << 30)
#define E1K_PHY_ADDR     1u           /* the 82574's internal PHY */

/* PHY registers (IEEE 802.3 clause 22) */
#define MII_BMCR         0
#define MII_ANAR         4
#define BMCR_AUTOEN      0x1000u
#define BMCR_RESTART     0x0200u
#define ANAR_PAUSE       0x0400u      /* symmetric pause: never advertised */
#define ANAR_PAUSE_ASYM  0x0800u      /* asymmetric pause: never advertised */

/* ICR / IMS bits (10.2.4) */
#define ICR_TXDW         (1u << 0)    /* a transmit descriptor written back */
#define ICR_LSC          (1u << 2)    /* link status change */
#define ICR_RXDMT0       (1u << 4)    /* receive descriptors running low */
#define ICR_RXO          (1u << 6)    /* receive overrun */
#define ICR_RXT0         (1u << 7)    /* receive timer: frames received */
#define ICR_RXQ0         (1u << 20)   /* MSI-X: receive queue 0 */
#define ICR_TXQ0         (1u << 22)   /* MSI-X: transmit queue 0 */
#define ICR_OTHER        (1u << 24)   /* MSI-X: link and the rest */
#define E1K_IMS_ALL      (ICR_TXDW | ICR_LSC | ICR_RXDMT0 | ICR_RXO | ICR_RXT0 | ICR_RXQ0 | \
                          ICR_TXQ0 | ICR_OTHER)

/* IVAR (10.2.4.9): a 4-bit entry per cause, vector in bits 2:0, bit 3
 * valid. Receive queue 0 in 3:0, transmit queue 0 in 11:8, other causes
 * (the link) in 19:16; bit 31 raises a transmit interrupt per written-back
 * descriptor. All on vector 0, the only one devmgr makes (em's
 * em_if_msix_intr_assign for the 82574 builds it the same way). */
#define IVAR_VALID       0x8u
#define E1K_IVAR_VEC0    (IVAR_VALID << 0 | IVAR_VALID << 8 | IVAR_VALID << 16 | 1u << 31)

/* RCTL bits (10.2.5.1) */
#define RCTL_EN          (1u << 1)
#define RCTL_BAM         (1u << 15)   /* accept broadcasts */
#define RCTL_SECRC       (1u << 26)   /* strip the CRC: lengths are without it */
/* Never set: SBP (bad frames), UPE/MPE (promiscuous), LPE (long frames),
 * VFE and CFIEN (the VLAN filter), BSEX (buffer sizes): BSIZE 0 is 2048. */
#define RCTL_VALUE       (RCTL_EN | RCTL_BAM | RCTL_SECRC)

/* TCTL bits (10.2.6.1): enable, pad short frames, the collision
 * threshold and distance em uses (E1000_COLLISION_THRESHOLD 15,
 * E1000_COLLISION_DISTANCE 63). */
#define TCTL_EN          (1u << 1)
#define TCTL_PSP         (1u << 3)
#define TCTL_VALUE       (TCTL_EN | TCTL_PSP | 15u << 4 | 63u << 12)
#define TIPG_VALUE       (8u | 8u << 10 | 6u << 20)   /* em: IPGT 8, IPGR1 8, IPGR2 6 */
/* TXDCTL: bit 22 must be 1 on the 82574 (10.2.6.10), descriptor
 * granularity, write back after each descriptor (em_initialize_transmit_unit). */
#define TXDCTL_VALUE     (1u << 22 | 1u << 24 | 1u << 16)

#define RAH_AV           (1u << 31)   /* the address is valid */

/* ---- descriptors (legacy formats: 7.1.4 receive, 7.2.10.1 transmit) ---------------- */

#define RX_DESCS         256
#define TX_DESCS         256
#define DESC_SIZE        16
#define BUF_SIZE         2048         /* RCTL.BSIZE 0; one frame per buffer */
#define RX_RING_OFF      0
#define TX_RING_OFF      (RX_DESCS * DESC_SIZE)
#define RINGS_BYTES      (TX_RING_OFF + TX_DESCS * DESC_SIZE)   /* 8 KiB, one VMO */
#define RX_BUF_OFF       0
#define TX_BUF_OFF       (RX_DESCS * BUF_SIZE)
#define BUFS_BYTES       (TX_BUF_OFF + TX_DESCS * BUF_SIZE)     /* 1 MiB */
#define BUF_PAGES        (BUFS_BYTES / 4096)

/* Receive descriptor: buffer address (0), length (8), checksum (10),
 * status (12), errors (13), special (14). */
#define RXD_ADDR         0
#define RXD_LEN          8
#define RXD_STATUS       12
#define RXD_ERRORS       13
#define RXD_DD           0x01         /* the chip is done with it */
#define RXD_EOP          0x02         /* the frame's last buffer */
#define RXD_ERR_MASK     0x97         /* CE, SE, SEQ, CXE, RXE (not the checksum bits) */

/* Transmit descriptor: buffer address (0), length (8), CSO (10), CMD
 * (11), STA (12), CSS (13), special (14: the VLAN, used only with VLE). */
#define TXD_ADDR         0
#define TXD_LEN          8
#define TXD_CMD          11
#define TXD_STA          12
#define TXD_CMD_EOP      0x01
#define TXD_CMD_IFCS     0x02         /* the chip adds the CRC */
#define TXD_CMD_RS       0x08         /* report status: DD set when sent */
#define TXD_CMD_VLE      0x40         /* hardware tag insertion: NEVER set */
#define TXD_STA_DD       0x01

/* ---- state ------------------------------------------------------------------------- */

#define CHIP_NAME        "82574L"

/* The chip's counters, added up (they clear on read). */
struct chip_counts {
    uint64_t rx_ok, tx_ok, rx_err, tx_err, missed, pause_sent;
};

struct e1k {
    volatile void *r;             /* BAR 0, the registers */
    handle_t dev, dma, irq, serve, port;
    uint16_t vid, did;            /* PCI ids, for the log */
    uint16_t vlan;                /* 1..4094: every frame's tag (never 0 once running) */
    uint8_t  mac[6];
    /* DMA memory: both descriptor rings in one VMO, every buffer in another */
    handle_t ring_vmo, buf_vmo;
    uint8_t *ring, *bufs;         /* mapped */
    uint64_t ring_pin, buf_pin;
    bool     ring_pinned, buf_pinned, bus_master;
    uint64_t ring_addr;           /* the rings' device address */
    uint64_t buf_addr[BUF_PAGES]; /* each buffer page's device address */
    uint32_t rx_next;             /* the next receive descriptor to look at */
    bool     rx_split;            /* inside a frame that spans buffers: drop to its end */
    uint32_t tx_prod, tx_cons;    /* transmit descriptors filled / reaped (free-running) */
    uint32_t tx_tail;             /* tx_prod as last written to TDT (tx_flush) */
    bool     tx_on;               /* the transmitter is on (tx_enable) */
    /* the link */
    bool     link, full;
    uint32_t speed;               /* Mb/s, 0 while down */
    uint32_t link_changes;        /* since the driver started: netdev.info's `changes` */
    uint32_t link_lines;          /* link lines logged (capped) */
    uint64_t started;             /* driver start, uptime ns */
    /* the netdev server (netserver.c) and the counts only the driver has */
    struct srv v;
    uint64_t rx_drop[NETFRAME_RX_KINDS];   /* by netframe_rx_check's reason */
    uint64_t rx_errors;           /* flagged by the chip, spread over buffers, or too long */
    uint64_t tx_done;             /* descriptors the chip reported sent */
    struct chip_counts chip;
    uint64_t irqs, polls;
    uint32_t icr_seen;            /* every ICR bit seen */
    uint8_t  frame[NETDEV_FRAME_MAX];      /* a kept frame, untagged, for srv_rx */
};

/* ---- chip.c ------------------------------------------------------------------------ */

uint32_t rd32(const struct e1k *t, uint32_t reg);
void     wr32(struct e1k *t, uint32_t reg, uint32_t v);
/* Stop whatever a previous driver left running, then reset the chip
 * (bounded waits). ERR_TIMED_OUT (logged): it didn't come out of reset;
 * ERR_NOT_FOUND: its registers read all ones. */
status_t chip_reset(struct e1k *t);
/* The address from the EEPROM (RAL0/RAH0 if that fails); logged. */
status_t chip_read_mac(struct e1k *t);
/* Filters (our address, broadcasts), flow control off, VLAN offloads
 * off, interrupts routed to vector 0, autonegotiation without pause. */
void     chip_setup(struct e1k *t);
void     chip_irq_enable(struct e1k *t, bool on);
/* STATUS read; a change is counted, logged (capped) and returned. */
bool     chip_link_poll(struct e1k *t);
/* The chip's counters added to t->chip (call at least every few
 * seconds: they are 32 bits and clear on read). */
void     chip_counters(struct e1k *t);
/* Receiver and transmitter off, interrupts masked, the chip reset: no
 * DMA after it returns. */
void     chip_stop(struct e1k *t);

/* ---- ring.c ------------------------------------------------------------------------ */

/* The rings and buffers made and pinned (bus mastering must be on), the
 * receive ring armed and given to the chip. */
status_t ring_setup(struct e1k *t);
void     ring_free(struct e1k *t);
volatile uint8_t *ring_rx_desc(const struct e1k *t, uint32_t i);
volatile uint8_t *ring_tx_desc(const struct e1k *t, uint32_t i);
uint64_t ring_rx_buf_addr(const struct e1k *t, uint32_t i);
uint64_t ring_tx_buf_addr(const struct e1k *t, uint32_t i);

/* ---- rx.c -------------------------------------------------------------------------- */

/* Every frame the chip has handed back: kept (our VLAN, untagged, to
 * srv_rx) or dropped and counted. Returns how many. */
unsigned rx_harvest(struct e1k *t);

/* ---- tx.c (THE transmit path) ------------------------------------------------------ */

/* The transmit ring given to the chip and the transmitter on. */
status_t tx_enable(struct e1k *t);
void     tx_disable(struct e1k *t);
/* One untagged frame of len bytes (the netdev server's send): copied,
 * tagged and checked into the next descriptor, which is queued; the
 * doorbell waits for tx_flush. OK; ERR_INVALID_ARGS (refused by the tag
 * check); ERR_NO_RESOURCES (no free descriptor); ERR_BAD_STATE (the
 * transmitter is off); ERR_ACCESS_DENIED (the gate). */
status_t tx_send(struct e1k *t, const uint8_t *frame, size_t len);
/* Free transmit descriptors (sent ones are taken back first if none is). */
uint32_t tx_room(struct e1k *t);
/* One doorbell (TDT) for every descriptor tx_send queued since the last. */
void     tx_flush(struct e1k *t);
/* Sent descriptors taken back; returns how many. */
unsigned tx_reap(struct e1k *t);

/* ---- loop.c ------------------------------------------------------------------------ */

/* The driver's own port keys (the server's start at SRV_KEY_FIRST). */
#define KEY_IRQ     1u
#define KEY_SERVE   2u            /* DR_SERVE's peer closed: devmgr is stopping us */

status_t loop_run(struct e1k *t);
