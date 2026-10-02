/* tools/checknotx.sh's self-check: a gate that forgets the VLAN (rule 4).
 * Never compiled. */
static inline bool rtl_tx_allowed(enum rtl_mode mode, uint32_t vlan)
{
    return mode == RTL_MODE_FULL;
}
