/* The offer of a fetched build to init and init's answer (<update.h>, the
 * protocol `update` in abi/idl/update.idl), between the generated call and
 * <update.h>'s struct update_answer: the one place that maps the call's
 * arguments and results to and from it, for bin/update and the test
 * programs that offer (update_make_offer) and for init (update_answer_reply).
 * The answer's text is data from a network's manifest and a stick's boot
 * menu, so init's side makes it fit the protocol's strings rather than
 * ever failing to answer. */
#include <idl/update.h>
#include <update.h>

/* abi/idl/update.idl's string caps are numbers there; the struct's arrays are these. */
_Static_assert(UPDATE_VERSION_MAX == 47 && UPDATE_GIT_MAX == 47 && UPDATE_NET_MAX == 15 &&
               UPDATE_EXT_KEY_MAX == 32 && UPDATE_MENU_WHY_MAX == 95 &&
               UPDATE_MANIFEST_MAX == 1024 && UPDATE_PARTS == 3,
               "abi/idl/update.idl's caps and <update.h>'s");

/* s, at most max bytes, into out as a string for the protocol: printable
 * ASCII only (anything else '?'). Returns its length. */
static uint32_t clean(char *out, const char *s, uint32_t max)
{
    uint32_t n = 0;
    for (; n < max && s[n]; n++)
        out[n] = s[n] >= ' ' && s[n] <= '~' ? s[n] : '?';
    return n;
}

status_t update_make_offer(handle_t ch, uint64_t deadline_ns, uint32_t flags,
                           const uint64_t bytes[UPDATE_PARTS], const void *manifest,
                           uint32_t mlen, const handle_t *files, uint32_t nfiles,
                           struct update_answer *a)
{
    char version[UPDATE_VERSION_MAX + 1], git[UPDATE_GIT_MAX + 1], net[UPDATE_NET_MAX + 1];
    char running[UPDATE_NET_MAX + 1], needs[UPDATE_EXT_KEY_MAX + 1], why[UPDATE_MENU_WHY_MAX + 1];
    uint32_t lv, lg, ln, lr, lneeds, lwhy;
    *a = (struct update_answer){ 0 };
    struct update_answer r = { 0 };
    status_t st = update_offer_until(ch, deadline_ns, flags, bytes[UPDATE_KERNEL],
                                     bytes[UPDATE_BOOTFS], bytes[UPDATE_MENU], manifest, mlen,
                                     files, nfiles, &r.status, &r.why, &r.file, &r.check_ms,
                                     version, &lv, git, &lg, &r.write_step, &r.stick,
                                     &r.write_ms, net, &ln, running, &lr, needs, &lneeds,
                                     &r.menu, &r.menu_status, why, &lwhy, &r.already);
    if (st != OK)
        return st;
    memcpy(r.version, version, lv);
    memcpy(r.git, git, lg);
    memcpy(r.net, net, ln);
    memcpy(r.net_running, running, lr);
    memcpy(r.needs, needs, lneeds);
    memcpy(r.menu_why, why, lwhy);
    *a = r;   /* the strings' ends: r was zeroed */
    return OK;
}

status_t update_answer_reply(handle_t ch, uint32_t txid, const struct update_answer *a)
{
    char version[UPDATE_VERSION_MAX], git[UPDATE_GIT_MAX], net[UPDATE_NET_MAX];
    char running[UPDATE_NET_MAX], needs[UPDATE_EXT_KEY_MAX], why[UPDATE_MENU_WHY_MAX];
    struct idl_txn txn = { .ch = ch, .txid = txid };
    uint32_t lv = clean(version, a->version, sizeof(version));
    uint32_t lg = clean(git, a->git, sizeof(git));
    uint32_t ln = clean(net, a->net, sizeof(net));
    uint32_t lr = clean(running, a->net_running, sizeof(running));
    uint32_t lneeds = clean(needs, a->needs, sizeof(needs));
    uint32_t lwhy = clean(why, a->menu_why, sizeof(why));
    return update_reply_offer(txn, OK, a->status, a->why, a->file, a->check_ms, version, lv, git,
                              lg, a->write_step, a->stick, a->write_ms, net, ln, running, lr,
                              needs, lneeds, a->menu, a->menu_status, why, lwhy, a->already);
}
