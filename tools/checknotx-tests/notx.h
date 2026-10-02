/* tools/checknotx.sh's self-check: a gate that lets any non-zero mode
 * through, not only a VLAN or untagged (rule 4). Never compiled. */
static inline bool rtl_tx_allowed(enum rtl_mode mode, uint32_t vlan)
{
    return mode == RTL_MODE_FULL && vlan != 0;
}
