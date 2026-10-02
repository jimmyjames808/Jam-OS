/* tools/checknotx.sh's self-check: a probe that reaches the transmit
 * path (rule 5). Never compiled. */
void probe_run(struct rtl *t, struct outcome *o)
{
    (void)tx_send(t, frame, 60);                          /* rule 5 */
}
