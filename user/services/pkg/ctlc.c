/* init's pkgctl channel (abi/idl/pkgctl.idl) as the transaction code calls
 * it: approve a package's staged programs, take back a version's lines.
 * init does the checking (the manifest's signature, the files it reads
 * back itself); this only carries the manifest and the answer. The
 * manifest goes in a VMO of its own that init reads and no one else holds. */
#include <idl/pkgctl.h>
#include "pkg.h"

#define APPROVE_WAIT (600 * NS_PER_S)   /* init reads and hashes the programs: gigabytes of them */
#define REVOKE_WAIT  (30 * NS_PER_S)

_Static_assert(PK_APPROVE_STAGED == PKGCTL_STAGED && PK_APPROVE_JAM == PKGCTL_JAM,
               "pkg.h's flags are pkgctl's");

static status_t approve(void *ctx, uint32_t flags, const void *text, size_t len,
                        struct pk_verdict *v)
{
    handle_t ch = *(handle_t *)ctx, vmo, dup;
    char why[PKGCTL_WHY_MAX + 1];
    uint32_t why_len = 0;
    int32_t verdict = 0;
    uint64_t pages = ((uint64_t)len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (!ch)
        return ERR_PEER_CLOSED;
    status_t st = jam_vmo_create(pages ? pages : PAGE_SIZE, 0, &vmo);
    if (st == OK)
        st = jam_vmo_write(vmo, 0, text, len);
    if (st == OK)
        st = jam_handle_duplicate(vmo, RIGHT_READ | RIGHT_TRANSFER, &dup);
    jam_handle_close(vmo);
    if (st != OK)
        return st;
    /* the call moves dup, whatever its status */
    st = pkgctl_approve_until(ch, now() + APPROVE_WAIT, flags, len, dup, &verdict, &v->programs,
                              &v->program, why, &why_len);
    if (st != OK)
        return st;
    why[why_len < sizeof(why) ? why_len : sizeof(why) - 1] = '\0';
    v->verdict = verdict;
    snprintf(v->why, sizeof(v->why), "%s", why);
    return OK;
}

static status_t revoke(void *ctx, const char *name, const char *version, uint32_t revision,
                       uint32_t *lines)
{
    handle_t ch = *(handle_t *)ctx;
    if (!ch)
        return ERR_PEER_CLOSED;
    return pkgctl_revoke_until(ch, now() + REVOKE_WAIT, name, (uint32_t)strlen(name), version,
                               (uint32_t)strlen(version), revision, lines);
}

void pk_ctl_make(struct pk_ctl *c, handle_t *ch)
{
    *c = (struct pk_ctl){ ch, approve, revoke };
}
