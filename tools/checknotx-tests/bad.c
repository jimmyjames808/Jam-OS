/* tools/checknotx.sh's self-check: each line below breaks one of its
 * rules, and the check must catch every one. Never compiled. */
static void bad(struct rtl *t, uint64_t ring)
{
    drv_write32(t->r, 0x38, 0);                           /* rules 1 and 7: past the guard */
    wr32(t, RTL_TXDESC_LO, (uint32_t)ring);               /* rule 2: a transmit ring */
    wr16(t, 0x90, 1);                                     /* rule 2: the doorbell by number */
    wr8(t, 0x57, 0x10);                                   /* rule 2: TDFNR by number */
    wr8(t, RTL_CMD, RTL_CMD_RXENB | RTL_CMD_TXENB);       /* rule 3: the transmitter on */
    t->mode = RTL_MODE_FULL;                              /* rule 6: the mode set elsewhere */
    *(volatile uint16_t *)((uint8_t *)t->r + 0x90) = 1;   /* rule 7: a register, raw */
    t->txbufs[0] = 0;                                     /* rule 7: a transmit buffer */
}
