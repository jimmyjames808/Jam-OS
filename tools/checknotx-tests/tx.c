/* tools/checknotx.sh's self-check: a tx.c that breaks rules 1 and 4.
 * Never compiled. */
static bool gate(struct rtl *t, const char *what)
{
    if (t->mode == RTL_MODE_FULL)                         /* rule 4: the gate without the VLAN */
        return true;
    return false;
}

static void txw16(struct rtl *t, uint32_t reg, uint16_t v)
{
    drv_write16(t->r, reg, v);                            /* rule 1: no gate before the write */
}

status_t tx_send(struct rtl *t, const uint8_t *frame, size_t len)
{
    txw16(t, RTL_TXSTART, 1);                             /* rule 4: an entry without the gate */
    return OK;
}

unsigned tx_reap(struct rtl *t,                          /* rule 4: a signature the check */
                 bool hidden)                             /* can't follow to its body */
{
    txw16(t, RTL_TXSTART, 1);
    return 0;
}
