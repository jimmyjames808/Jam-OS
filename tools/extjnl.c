/* extcheck's tests of the journal (user/services/extfs/ext/ext_jnl.h).
 *
 * The cut tests: each storm (tools/extcut.c) runs once uncut, recording the
 * state of its objects after every commit (snapshot k: after k commits)
 * and how many blocks it writes, N. Then for every n from 1 to N it runs
 * again on a fresh copy of the fixture with the device cut after its nth
 * block (the write that crosses the cut torn), and the volume is mounted
 * again as a successor would (ext_jnl_mount, the replay). It must:
 *   - mount writable, journal open;
 *   - hold exactly snapshot d, where d is the number of commits whose
 *     commit block reached the device in the cut run: everything synced
 *     before the cut is there, nothing of a transaction that wasn't is
 *     (a cut can only land a commit block in a write that also lands the
 *     rest of its transaction or is checked against it), except blocks
 *     written as file data around the journal, which must hold the last
 *     such write that landed (a replay must never write over them);
 *   - say ext_jnl_committed of the cut run's last commit id exactly when
 *     that commit is in d (the service's request slot rule);
 *   - list every file the storm did not touch exactly as Linux's debugfs
 *     listed the fixture (every 4th cut: it reads every file), with every
 *     bitmap whole;
 *   - go on: two more commits, a clean close (needs_recovery cleared, the
 *     journal empty on the disk), and a mount after it that replays
 *     nothing and finds them.
 * Storms on both writable fixtures, with the commit block in its own
 * write and with one write per transaction (torn writes landing any of
 * their blocks), and on journals shrunk so the log wraps and the tail
 * moves under every kind of cut; and on a device with a volatile cache (a
 * USB stick's: a commit counts once the flush after its commit block
 * returned, and of what was written since the last flush any part may be
 * on the medium after the cut), where the journal's flushes are all that
 * keeps its order.
 *
 * Then: every log block a storm left that begins with jbd2's magic is a
 * real header (copies that begin with it are escaped); damaged log blocks
 * never make a replay write outside the volume or into the log, nor
 * crash it; a damaged journal superblock or one with unknown features
 * mounts the volume read-only; a fresh mount sets checksum v3 and
 * needs_recovery and a close clears it; a transaction too big for the
 * journal is refused and given up whole; a replay whose revoke records
 * don't fit leaves the volume read-only and still needing recovery.
 *
 * With --cut-images DIR, three cut images of each storm are written for
 * the PC's e2fsck (tools/extfix/cutcheck.sh): <storm>-<x>-cut.img as the
 * cut left it, <storm>-<x>-ours.img after this library's recovery and a
 * clean close, <storm>-<x>.list its listing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "extjnl.h"
#include "extwrite.h"
#include "jnl_int.h"

#define LIST_EVERY 4   /* cuts between two full listings */

/* The first seven keep the log long (the tail moves by commits only), so
 * their cuts replay many transactions; the last two have the library's own
 * tail (every few blocks), whose cuts replay one or two. */
static const struct storm_cfg storms[] = {
    { "jam4k", 0, 0, true, 0, 0, 24, 0x5101, 0, UINT32_MAX, false },
    { "jam4k-onewrite", 0, EXT_JNL_ONE_WRITE, true, 0, 0, 24, 0x5102, 0, UINT32_MAX, false },
    { "jam4k-wrap", 0, 0, true, 48, 4, 40, 0x5103, 0, UINT32_MAX, false },
    { "linux1k", 1, 0, true, 0, 0, 24, 0x5104, 0, UINT32_MAX, false },
    { "linux1k-wrap-onewrite", 1, EXT_JNL_ONE_WRITE, true, 64, 0, 30, 0x5105, 0, UINT32_MAX, false },
    { "names4k", 0, 0, true, 0, 0, 100, 0x5106, 4096, UINT32_MAX, false },
    { "names1k-onewrite", 0, EXT_JNL_ONE_WRITE, true, 0, 4, 100, 0x5107, 1024, UINT32_MAX, false },
    { "jam4k-tail", 0, 0, true, 0, 0, 24, 0x5108, 0, 0, false },
    { "linux1k-tail-onewrite", 1, EXT_JNL_ONE_WRITE, true, 0, 0, 24, 0x5109, 0, 0, false },
    /* A device with a volatile cache (a USB stick): what reaches the
     * medium is only what was flushed, and any of the rest. */
    { "jam4k-cache", 0, 0, true, 0, 0, 24, 0x510a, 0, 0, true },
    { "linux1k-cache-onewrite", 1, EXT_JNL_ONE_WRITE, true, 0, 0, 24, 0x510b, 0, 0, true },
    { "jam4k-wrap-cache", 0, 0, true, 48, 4, 40, 0x510c, 0, UINT32_MAX, true },
};

/* What one cut run left. */
struct cut {
    uint32_t durable;             /* commits whose commit block landed */
    bool attempted;               /* a commit was started */
    uint32_t last_tid;            /* the last one started */
    bool in_checkpoint;           /* it stopped after a commit landed, before it was home */
    uint64_t landed[STORM_OBJS];
    bool has_landed[STORM_OBJS];
};

/* ---- one cut ------------------------------------------------------------------------------- */

/* Run the storm on the fixture with the device cut after n blocks (0: no
 * cut); the device then works again, as after a power cut. */
static void cut_run(struct storm *s, uint64_t n, struct cut *c)
{
    cut_reset(&s->cd);
    s->cd.cut_at = n;
    s->cd.reorder = s->cfg->reorder;
    rng = (s->cfg->seed ^ (n * 0x9E3779B97F4A7C15ull)) | 1;
    struct run r;
    memset(c, 0, sizeof(*c));
    if (run_mount(s, &r) == OK && r.fs.jnl) {
        storm_run(s, &r, false);
        c->durable = r.j.tid - r.tid0;
        c->attempted = r.attempted;
        c->last_tid = r.last_tid;
        c->in_checkpoint = r.why_stop != OK && r.attempted && r.last_tid != r.j.tid;
        memcpy(c->landed, r.landed, sizeof(c->landed));
        memcpy(c->has_landed, r.has_landed, sizeof(c->has_landed));
        /* With a cache, a data write that returned may not be on the
         * medium: what is there after the cut is what a replay must keep. */
        for (uint32_t i = 0; s->cd.cache && s->cd.dead && i < s->nobj; i++)
            if (c->has_landed[i])
                c->landed[i] = block_hash(s->cd.bytes + s->objs[i].blk * s->cd.block_size,
                                          s->cd.block_size);
    }
    run_free(&r);
    s->cd.cut_at = 0;
    s->cd.dead = false;
}

/* Does the recovered state equal snapshot k (blocks written as data
 * holding their last write that landed)? *at: the first object that
 * doesn't. */
static bool state_is(const struct storm *s, const struct cut *c, const uint64_t *got, uint32_t k,
                     uint32_t *at)
{
    for (uint32_t i = 0; i < s->nobj; i++)
        if (got[i] != (c->has_landed[i] ? c->landed[i] : s->snap[k][i])) {
            *at = i;
            return false;
        }
    return true;
}

/* The recovered state: snapshot c->durable, or the one after it if the
 * commit the cut interrupted counts (every byte of it is on the disk,
 * as when its blocks that did not land already held the same bytes).
 * The request slot's rule with it: ext_jnl_committed says so exactly
 * when it counts. */
static void check_state(struct storm *s, struct run *q, const struct cut *c, uint32_t tid0)
{
    uint64_t got[STORM_OBJS];
    uint32_t at = 0, k = c->durable;
    storm_state(s, q, got);
    if (k >= s->nsnap)
        fail("%u commits landed of %u", k, s->nsnap - 1);
    bool inflight = c->attempted && c->last_tid == tid0 + k;
    if (!state_is(s, c, got, k, &at)) {
        uint32_t i = at;
        uint64_t want = c->has_landed[i] ? c->landed[i] : s->snap[k][i];
        if (!inflight || k + 1 >= s->nsnap || !state_is(s, c, got, k + 1, &at))
            fail("object %u (kind %u%s): after %u commits it must be %#llx, it is %#llx", i,
                 s->objs[i].kind, c->has_landed[i] ? ", written as data" : "", k,
                 (unsigned long long)want, (unsigned long long)got[i]);
        k++;
    }
    if (inflight && ext_jnl_committed(&q->fs, c->last_tid) != (k > c->durable))
        fail("commit %u: committed says %d, the volume %d", c->last_tid, k == c->durable,
             k > c->durable);
    if (ext_jnl_committed(&q->fs, tid0 + k))
        fail("commit %u was never written but counts as committed", tid0 + k);
    if (k && !ext_jnl_committed(&q->fs, tid0 + k - 1))
        fail("commit %u landed but doesn't count", tid0 + k - 1);
}

/* Is this listing line one the storm changes (an inode it touches, a name
 * it exchanges)? */
static bool touched(const struct storm *s, const char *line)
{
    const char *tab = strchr(line, '\t');
    unsigned long ino = tab && tab[1] && tab[2] == '\t' ? strtoul(tab + 3, NULL, 10) : 0;
    size_t plen = tab ? (size_t)(tab - line) : strlen(line);
    if (ino == s->holes_ino)
        return true;
    for (uint32_t i = 0; i < s->nobj; i++) {
        const struct obj *o = &s->objs[i];
        if ((o->kind == OBJ_INODE || o->kind == OBJ_FILE) && o->ino == ino)
            return true;
        if (o->kind != OBJ_SWAP || plen < 6 || strncmp(line, "/many/", 6))
            continue;
        if ((strlen(o->na) == plen - 6 && !strncmp(line + 6, o->na, plen - 6)) ||
            (strlen(o->nb) == plen - 6 && !strncmp(line + 6, o->nb, plen - 6)))
            return true;
    }
    return false;
}

static void filtered(const struct storm *s, const struct lines *in, struct lines *out)
{
    out->n = 0;
    for (unsigned i = 0; i < in->n; i++)
        if (!touched(s, in->v[i]))
            out->v[out->n++] = in->v[i];
}

/* Every file the storm leaves alone, as debugfs listed the fixture. */
static void check_untouched(struct storm *s, struct run *q, const struct lines *want)
{
    static struct lines got, a, b;
    list_volume(&q->fs, &got);
    filtered(s, want, &a);
    filtered(s, &got, &b);
    compare_lines(&a, &b);
    free_lines(&got);
    check_bitmaps(&q->fs);
    verify_volume(&q->fs);
}

/* Two more commits, a clean close, a mount that replays nothing. */
static void check_goes_on(struct storm *s, struct run *q)
{
    struct ext_fs *fs = &q->fs;
    bool names = s->cfg->mkfs_bs;
    uint32_t blk_obj = names ? 0 : s->o_block, ino = names ? EXT_ROOT_INO : s->objs[s->o_root].ino;
    struct ext_inode in;
    struct ext_buf *b;
    status_t st = ext_jnl_begin(fs, NULL);
    fs->sb.wtime = 0xC0FFEE;
    if (st == OK)
        st = ext_super_write(fs);
    if (st == OK && !names)
        st = ext_bnew(fs, s->objs[blk_obj].blk, &b);
    if (st == OK && !names) {
        storm_pattern(b->data, fs->block_size, 0xABCD);
        ext_bdirty(fs, b);
        ext_brelse(fs, b);
        st = ext_jnl_commit(fs);
    }
    if (st == OK)
        st = ext_iget(fs, ino, 0, &in);
    if (st == OK) {
        struct ext_time t = { .sec = 1234567890, .nsec = 0 };
        ext_itime_set(&in, EXT_MTIME, &t);
        st = ext_iput(fs, &in);
    }
    if (st == OK)
        st = ext_jnl_commit(fs);
    if (st == OK)
        st = ext_jnl_close(fs);
    if (st != OK)
        fail("after the recovery: %d (%s)", st, fs->why ? fs->why : "");
    const struct ext_super *sb = (const void *)(s->cd.bytes + EXT_SUPER_OFFSET);
    if (sb->feature_incompat & EXT_INCOMPAT_RECOVER)
        fail("a clean close left needs_recovery set");
    run_free(q);
    if (run_mount(s, q) != OK || q->fs.ro || q->j.replayed || q->fs.sb.wtime != 0xC0FFEE)
        fail("the mount after a clean close: ro %#x, replayed %u", q->fs.ro, q->j.replayed);
    uint8_t want[4096];
    storm_pattern(want, fs->block_size, 0xABCD);
    struct ext_time t;
    if (names)
        return;
    if (memcmp(s->cd.bytes + (s->objs[blk_obj].blk << q->fs.block_log), want,
               q->fs.block_size) || ext_iget(&q->fs, ino, 0, &in) != OK ||
        (ext_itime(&in, EXT_MTIME, &t), t.sec != 1234567890))
        fail("the commits after the recovery are not there");
}

/* Mount what the cut left and check it all. */
static void check_recovered(struct storm *s, uint64_t n, const struct cut *c,
                            const struct lines *want, uint32_t tid0)
{
    struct run q;
    status_t st = run_mount(s, &q);
    if (st != OK || q.fs.ro || !q.fs.jnl)
        fail("cut after %llu blocks: the mount: %d, ro %#x (%s)", (unsigned long long)n, st,
             q.fs.ro, q.fs.why ? q.fs.why : "");
    check_state(s, &q, c, tid0);
    uint32_t every = s->cfg->tail_every ? s->cfg->tail_every : EXT_JNL_TAIL_EVERY;
    if (q.j.replayed > every)
        fail("%u transactions replayed: the tail moves every %u", q.j.replayed, every);
    /* The library's own tail moves once the log holds EXT_JNL_TAIL_BLOCKS
     * blocks: a cut leaves the one transaction in hand and at most one
     * before it (smaller than that), so a start replays two at most. */
    if (!s->cfg->tail_blocks && q.j.replayed > 2)
        fail("%u transactions replayed: the tail moves every %u blocks", q.j.replayed,
             EXT_JNL_TAIL_BLOCKS);
    if (n % LIST_EVERY == 0 && want) {
        check_untouched(s, &q, want);
    } else if (n % LIST_EVERY == 0) {
        struct inv inv;
        check_invariants(&q.fs, &inv);
        verify_volume(&q.fs);
    }
    check_goes_on(s, &q);
    run_free(&q);
}

/* ---- the log a storm leaves ----------------------------------------------------------------- */

/* Every log block that begins with jbd2's magic is a header whose checksum
 * holds: a copy that begins with it was escaped. Returns the escaped tags
 * seen. */
static uint32_t check_hygiene(struct run *r)
{
    struct ext_fs *fs = &r->fs;
    struct ext_jnl *j = &r->j;
    uint8_t buf[4096];
    uint32_t escaped = 0;
    for (uint32_t pos = j->first; pos < j->maxlen; pos++) {
        if (jnl_read(fs, j, pos, buf) != OK)
            fail("log block %u: read", pos);
        if (!jnl_magic_at(buf))
            continue;
        uint32_t type = jbd2_be32(((struct jbd2_header *)(void *)buf)->blocktype);
        bool ok = type == JBD2_COMMIT ? jnl_commit_ok(fs, j, buf) :
                  type == JBD2_DESCRIPTOR || type == JBD2_REVOKE ? jnl_tail_ok(fs, j, buf) : false;
        if (!ok)
            fail("log block %u begins with jbd2's magic but is no header (type %u)", pos, type);
        for (uint32_t at = sizeof(struct jbd2_header);
             type == JBD2_DESCRIPTOR && at + j->tag_bytes <= fs->block_size - 4;) {
            struct jbd2_tag3 t;
            memcpy(&t, buf + at, sizeof(t));
            uint32_t flags = jbd2_be32(t.flags);
            escaped += !!(flags & JBD2_FLAG_ESCAPE);
            at += j->tag_bytes + (flags & JBD2_FLAG_SAME_UUID ? 0 : JBD2_UUID_SIZE);
            if (flags & JBD2_FLAG_LAST_TAG)
                break;
        }
    }
    return escaped;
}

/* ---- a storm ------------------------------------------------------------------------------- */

static char where_buf[160];

/* The kinds of cut a storm must have shown, counted. */
struct cut_counts {
    unsigned at_mount;       /* before the journal was open */
    unsigned in_checkpoint;  /* a commit landed, not yet home */
    unsigned torn_commit;    /* a commit started, not landed */
    unsigned other;          /* between requests, in a tail move, in a data write */
};

static void count(const struct cut *c, struct cut_counts *k, uint32_t tid0)
{
    if (!c->attempted && !c->durable)
        k->at_mount++;
    else if (c->in_checkpoint)
        k->in_checkpoint++;
    else if (c->attempted && c->last_tid == tid0 + c->durable)
        k->torn_commit++;
    else
        k->other++;
}

static void write_file(const char *dir, const char *name, const char *tag, const char *ext,
                       const void *p, size_t n)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s-%s%s", dir, name, tag, ext);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(p, 1, n, f) != n || fclose(f))
        fail("can't write %s", path);
}

/* The PC's images of cut n: as cut, after this library's recovery and a
 * clean close, and that one's listing. */
static void save_images(struct storm *s, uint64_t n, const char *tag, const char *dir)
{
    struct cut c;
    struct run q;
    cut_run(s, n, &c);
    write_file(dir, s->cfg->name, tag, "-cut.img", s->cd.bytes, s->cd.size);
    if (run_mount(s, &q) != OK || q.fs.ro || ext_jnl_close(&q.fs) != OK)
        fail("images: the recovery");
    write_file(dir, s->cfg->name, tag, "-ours.img", s->cd.bytes, s->cd.size);
    static struct lines l;
    list_volume(&q.fs, &l);
    size_t len = 0;
    for (unsigned i = 0; i < l.n; i++)
        len += strlen(l.v[i]) + 1;
    char *text = malloc(len + 1), *p = text;
    for (unsigned i = 0; text && i < l.n; i++)
        p += snprintf(p, len + 1 - (size_t)(p - text), "%s\n", l.v[i]);
    write_file(dir, s->cfg->name, tag, ".list", text, len);
    free(text);
    free_lines(&l);
    run_free(&q);
}

/* Cut the storm after every block it writes; the PC's images if asked. */
static void check_cuts(struct storm *s, const struct lines *want, uint32_t tid0, const char *images)
{
    struct cut_counts k = { 0 };
    uint64_t pick[3] = { 0, 0, 0 };
    for (uint64_t n = 1; n <= s->units; n++) {
        snprintf(where_buf, sizeof(where_buf), "%s, cut after block %llu of %llu", s->cfg->name,
                 (unsigned long long)n, (unsigned long long)s->units);
        where = where_buf;
        struct cut c;
        cut_run(s, n, &c);
        count(&c, &k, tid0);
        check_recovered(s, n, &c, want, tid0);
        if (!pick[0] && n >= s->units / 3 && c.in_checkpoint && c.durable >= 3)
            pick[0] = n;
        if (!pick[1] && n >= s->units / 2 && c.attempted && c.last_tid == tid0 + c.durable)
            pick[1] = n;
    }
    if (!k.at_mount || !k.in_checkpoint || !k.torn_commit || !k.other)
        fail("%s: the cuts missed a kind: %u at the mount, %u in a checkpoint, %u in a commit, "
             "%u else", s->cfg->name, k.at_mount, k.in_checkpoint, k.torn_commit, k.other);
    printf("extcheck: %s: %llu cuts (%u at the mount, %u in a checkpoint, %u in a commit, "
           "%u between): recovered whole\n", s->cfg->name, (unsigned long long)s->units,
           k.at_mount, k.in_checkpoint, k.torn_commit, k.other);
    if (!images)
        return;
    pick[2] = s->units;
    static const char *tags[3] = { "a", "b", "c" };
    for (int i = 0; i < 3; i++)
        if (pick[i])
            save_images(s, pick[i] == s->units ? 0 : pick[i], tags[i], images);
}

static void check_storm(struct img *fixtures, const struct storm_cfg *cfg, const char *listdir,
                        const char *images)
{
    static struct storm s;
    struct img *m = &fixtures[cfg->fixture];
    snprintf(where_buf, sizeof(where_buf), "%s, the run without a cut", cfg->name);
    where = where_buf;
    storm_make(&s, cfg, m);
    struct run r;
    if (run_mount(&s, &r) != OK || r.fs.ro || !r.fs.jnl)
        fail("the mount: ro %#x (%s)", r.fs.ro, r.fs.why ? r.fs.why : "");
    uint32_t tid0 = r.tid0;
    s.cd.jstart = r.j.runs[0].pblk;
    s.cd.jlen = r.j.maxlen;
    storm_run(&s, &r, true);
    if (r.why_stop != OK)
        fail("stopped: %d (%s)", r.why_stop, r.fs.why ? r.fs.why : "");
    s.units = s.cd.units;
    /* The default writes a commit block alone, after the rest is on the
     * disk; EXT_JNL_ONE_WRITE writes it with the rest. */
    bool one = cfg->jflags & EXT_JNL_ONE_WRITE;
    if (s.cd.commits_alone + s.cd.commits_with != s.nsnap - 1 ||
        (one ? s.cd.commits_alone : s.cd.commits_with))
        fail("%u commits: %llu written alone, %llu with other blocks", s.nsnap - 1,
             (unsigned long long)s.cd.commits_alone, (unsigned long long)s.cd.commits_with);
    uint32_t escaped = check_hygiene(&r);
    if (!cfg->maxlen && !cfg->mkfs_bs && !escaped)
        fail("no escaped copy in the log");
    run_free(&r);
    struct lines want = { .n = 0 };
    if (!cfg->mkfs_bs)
        read_list(listdir, m->name, &want);
    check_cuts(&s, cfg->mkfs_bs ? NULL : &want, tid0, images);
    free_lines(&want);
    cut_free(&s.cd);
}

/* ---- a damaged log ------------------------------------------------------------------------ */

/* The image a whole storm leaves (the journal still holding it) with
 * bytes of its log changed: the mount must neither crash nor write
 * outside the volume or into the log, and must say what it did. */
static void check_damaged_log(struct img *fixtures, const struct storm_cfg *cfg, unsigned runs)
{
    static struct storm s;
    snprintf(where_buf, sizeof(where_buf), "%s, a damaged log", cfg->name);
    where = where_buf;
    storm_make(&s, cfg, &fixtures[cfg->fixture]);
    struct run r;
    if (run_mount(&s, &r) != OK || !r.fs.jnl || r.j.nruns != 1)
        fail("the mount");
    storm_run(&s, &r, false);
    uint32_t bs = r.fs.block_size, first = r.j.first, len = r.j.maxlen - first;
    s.cd.jstart = r.j.runs[0].pblk;
    s.cd.jlen = r.j.maxlen;
    run_free(&r);
    cut_rebase(&s.cd);
    s.cd.watch = true;
    unsigned part = 0, ro = 0;
    rng = cfg->seed * 7 + 1;
    for (unsigned i = 0; i < runs; i++) {
        cut_reset(&s.cd);
        for (unsigned k = 1 + (unsigned)below(4); k; k--) {
            uint64_t off = ((s.cd.jstart + first + below(len)) * bs) + below(bs);
            uint8_t v = (uint8_t)(s.cd.bytes[off] ^ (1u << below(8)));
            cut_poke(&s.cd, off, &v, 1);
        }
        status_t st = run_mount(&s, &r);
        if (st != OK)
            fail("run %u: the mount: %d (%s)", i, st, r.fs.why ? r.fs.why : "");
        if (s.cd.bad_writes)
            fail("run %u: the replay wrote into the log", i);
        if (r.fs.ro) {
            ro++;
        } else {
            uint64_t got[STORM_OBJS];
            storm_state(&s, &r, got);
            check_bitmaps(&r.fs);
            part++;
        }
        run_free(&r);
    }
    s.cd.watch = false;
    printf("extcheck: %s: %u damaged logs: %u mounted (a prefix replayed), %u read-only\n",
           cfg->name, runs, part, ro);
    cut_free(&s.cd);
}

/* ---- the journal superblock, the mount and the close ------------------------------------- */

/* The journal superblock on the device. */
static struct jbd2_super *jsb_at(struct cutdev *cd, const struct run *r)
{
    return (struct jbd2_super *)(void *)(cd->bytes + (r->j.runs[0].pblk << r->fs.block_log));
}

static uint32_t jsb_csum(const struct jbd2_super *sb)
{
    struct jbd2_super t = *sb;
    t.checksum = 0;
    return ext_crc32c(~0u, &t, sizeof(t));
}

static bool recover_set(const struct cutdev *cd)
{
    const struct ext_super *sb = (const void *)(cd->bytes + EXT_SUPER_OFFSET);
    return sb->feature_incompat & EXT_INCOMPAT_RECOVER;
}

/* A fresh mount writes checksum v3 and needs_recovery; a close clears it
 * and leaves the journal empty; the next mount replays nothing and goes on
 * from the close's id. A read-only mount writes nothing. */
static void check_mount_close(struct storm *s)
{
    struct run r;
    cut_reset(&s->cd);
    if (run_mount(s, &r) != OK || r.fs.ro)
        fail("the mount: ro %#x", r.fs.ro);
    const struct jbd2_super *sb = jsb_at(&s->cd, &r);
    uint32_t want = JBD2_INCOMPAT_REVOKE | JBD2_INCOMPAT_CSUM_V3 |
                    (r.fs.is64 ? JBD2_INCOMPAT_64BIT : 0);
    if (jbd2_be32(sb->feature_incompat) != want || sb->checksum_type != JBD2_CRC32C ||
        jbd2_be32(sb->checksum) != jsb_csum(sb) || sb->feature_compat || sb->start)
        fail("the journal superblock after a mount: incompat %#x", jbd2_be32(sb->feature_incompat));
    if (!recover_set(&s->cd))
        fail("needs_recovery isn't set while mounted");
    uint32_t next = r.j.tid;
    if (ext_jnl_close(&r.fs) != OK || recover_set(&s->cd) || sb->start ||
        jbd2_be32(sb->sequence) != next || jbd2_be32(sb->checksum) != jsb_csum(sb))
        fail("the close");
    run_free(&r);
    if (run_mount(s, &r) != OK || r.fs.ro || r.j.replayed || r.j.mount_end != next ||
        ext_jnl_committed(&r.fs, next) || !ext_jnl_committed(&r.fs, next - 1))
        fail("the mount after a close");
    run_free(&r);
    cut_reset(&s->cd);
    struct ext_dev dev = cut_dev(&s->cd);
    uint8_t *cache = malloc(CACHE_BYTES), small[1024];
    struct ext_jnl j;
    if (ext_jnl_init(&j, small, sizeof(small), 0) != OK ||
        ext_jnl_mount(&r.fs, &j, &dev, cache, CACHE_BYTES, 0) != ERR_NO_MEMORY)
        fail("a journal region of 1 KiB isn't refused");
    if (ext_jnl_init(&j, cache, 0, 0x80) != ERR_INVALID_ARGS)
        fail("an unknown flag isn't refused");
    size_t jsize = ext_jnl_mem_size(4096, 1024, 64);
    uint8_t *jmem = malloc(jsize);
    if (ext_jnl_init(&j, jmem, jsize, 0) != OK ||
        ext_jnl_mount(&r.fs, &j, &dev, cache, CACHE_BYTES, EXT_MOUNT_READ_ONLY) != OK ||
        r.fs.jnl || s->cd.ntouched || ext_jnl_begin(&r.fs, NULL) != ERR_BAD_STATE)
        fail("a read-only mount wrote, or opened the journal");
    free(cache);
    free(jmem);
}

/* A damaged journal superblock, and one with a feature not known here,
 * mount the volume read-only, writing nothing. */
static void check_bad_jsb(struct storm *s)
{
    struct run r;
    cut_reset(&s->cd);
    if (run_mount(s, &r) != OK || ext_jnl_close(&r.fs) != OK)
        fail("the mount");
    uint64_t at = (uint64_t)((uint8_t *)jsb_at(&s->cd, &r) - s->cd.bytes);
    run_free(&r);
    cut_rebase(&s->cd);
    struct jbd2_super sb;
    memcpy(&sb, s->cd.bytes + at, sizeof(sb));
    uint8_t v = sb.uuid[3] ^ 1;
    cut_poke(&s->cd, at + __builtin_offsetof(struct jbd2_super, uuid) + 3, &v, 1);
    if (run_mount(s, &r) != OK || !(r.fs.ro & EXT_RO_DAMAGED) || r.fs.jnl ||
        !r.fs.why || strcmp(r.fs.why, "journal: superblock checksum") || s->cd.units)
        fail("a journal superblock with a bad checksum: ro %#x (%s)", r.fs.ro,
             r.fs.why ? r.fs.why : "");
    run_free(&r);
    cut_reset(&s->cd);
    sb.feature_incompat |= jbd2_be32(0x100);
    sb.checksum = jbd2_be32(jsb_csum(&sb));
    cut_poke(&s->cd, at, &sb, sizeof(sb));
    if (run_mount(s, &r) != OK || !(r.fs.ro & EXT_RO_FEATURES) || r.fs.jnl || !r.fs.why ||
        strcmp(r.fs.why, "journal: features not known here") || s->cd.units)
        fail("a journal superblock with an unknown feature: ro %#x (%s)", r.fs.ro,
             r.fs.why ? r.fs.why : "");
    run_free(&r);
    cut_reset(&s->cd);
}

/* A commit block as Linux writes it (no digest) still has every copy of
 * its transaction checked against its tag: a transaction committed and
 * cut before its checkpoint, its commit block's digest taken out and one
 * byte of its one copy changed, is not replayed. */
static void check_linux_commit(struct storm *s)
{
    struct run r;
    cut_reset(&s->cd);
    if (run_mount(s, &r) != OK || r.fs.ro)
        fail("the mount");
    struct ext_fs *fs = &r.fs;
    uint32_t ino = s->objs[s->o_root].ino, tid = r.j.tid;
    struct ext_inode in;
    struct ext_time before, t = { .sec = 1111111111, .nsec = 0 };
    s->cd.die_on_commit = true;
    s->cd.jstart = r.j.runs[0].pblk;
    s->cd.jlen = r.j.maxlen;
    if (ext_jnl_begin(fs, NULL) != OK || ext_iget(fs, ino, 0, &in) != OK)
        fail("the transaction");
    ext_itime(&in, EXT_MTIME, &before);
    ext_itime_set(&in, EXT_MTIME, &t);
    if (ext_iput(fs, &in) != OK || ext_jnl_commit(fs) != ERR_IO)
        fail("a commit cut before its checkpoint");
    s->cd.die_on_commit = false;
    s->cd.dead = false;
    /* The log from s_start: the descriptor, the inode's copy, the commit. */
    struct jbd2_super *sb = jsb_at(&s->cd, &r);
    uint64_t at = (r.j.runs[0].pblk + jbd2_be32(sb->start)) << fs->block_log;
    uint32_t bs = fs->block_size;
    struct jbd2_commit c;
    memcpy(&c, s->cd.bytes + at + 2 * bs, sizeof(c));
    if (jbd2_be32(c.h.blocktype) != JBD2_COMMIT || jbd2_be32(c.h.sequence) != tid)
        fail("no commit block where it should be");
    uint8_t block[4096];
    memcpy(block, s->cd.bytes + at + 2 * bs, bs);
    memset(block + __builtin_offsetof(struct jbd2_commit, chksum), 0, 4 * 4);
    jnl_commit_set(fs, &r.j, block);
    cut_poke(&s->cd, at + 2 * bs, block, bs);
    uint8_t v = s->cd.bytes[at + bs + 100] ^ 0x40;
    cut_poke(&s->cd, at + bs + 100, &v, 1);
    run_free(&r);
    if (run_mount(s, &r) != OK || r.fs.ro || r.j.replayed || ext_jnl_committed(&r.fs, tid) ||
        ext_iget(&r.fs, ino, 0, &in) != OK)
        fail("a torn copy under a commit without a digest: replayed %u, ro %#x (%s)",
             r.j.replayed, r.fs.ro, r.fs.why ? r.fs.why : "");
    ext_itime(&in, EXT_MTIME, &t);
    if (t.sec != before.sec)
        fail("the inode's mtime is %lld, not %lld", (long long)t.sec, (long long)before.sec);
    run_free(&r);
    cut_reset(&s->cd);
}

/* Cut /holes (its extent tree one level below the inode) to `size`, in
 * one transaction. */
static void cut_holes(struct run *r, uint64_t size, bool commit)
{
    struct ext_inode in;
    bool done = false;
    lookup(&r->fs, "holes", &in);
    status_t st = ext_jnl_begin(&r->fs, NULL);
    while (st == OK && !done)
        st = ext_truncate(&r->fs, &in, size, &done);
    if (st != OK || (commit && ext_jnl_commit(&r->fs) != OK))
        fail("cutting /holes to %llu: %d (%s)", (unsigned long long)size, st,
             r->fs.why ? r->fs.why : "");
}

/* U2a's frees reach the journal (ext_bfree calls ext_jnl_freed): a file
 * cut to nothing frees its extent node, which has a copy in the log (the
 * cut before changed it), and its data blocks, which never had one: one
 * revoke record. Once the tail has moved past that copy, none. */
static void check_freed(struct storm *s)
{
    for (int tail_moved = 0; tail_moved < 2; tail_moved++) {
        struct run r;
        cut_reset(&s->cd);
        if (run_mount(s, &r) != OK || r.fs.ro)
            fail("the mount");
        r.j.tail_blocks = UINT32_MAX;   /* only the checkpoint below moves the tail */
        struct ext_inode in;
        struct ext_path p;
        lookup(&r.fs, "holes", &in);
        uint64_t size = ext_isize(&in);
        if (ext_extent_find(&r.fs, &in, 0, &p) != OK || p.depth != 1)
            fail("/holes: no extent node");
        uint64_t node = p.lv[1].buf->blk;
        ext_path_release(&r.fs, &p);
        cut_holes(&r, (size / 2) & ~(uint64_t)(r.fs.block_size - 1), true);
        if (tail_moved && ext_jnl_checkpoint(&r.fs) != OK)
            fail("the checkpoint");
        cut_holes(&r, 0, false);
        uint32_t want = tail_moved ? 0 : 1;
        if (r.j.nrevokes != want || (want && r.j.revokes[0].blk != node))
            fail("%u blocks revoked, not %u (the node at %llu)%s", r.j.nrevokes, want,
                 (unsigned long long)node, tail_moved ? " after the tail moved" : "");
        if (ext_jnl_commit(&r.fs) != OK)
            fail("the commit");
        verify_volume(&r.fs);
        run_free(&r);
    }
    cut_reset(&s->cd);
}

/* ---- limits -------------------------------------------------------------------------------- */

/* A transaction larger than the journal takes is refused before anything
 * is written, and given up whole; ext_jnl_step refuses a step that can
 * never fit; a transaction within the room commits. */
static void check_too_big(struct storm *s)
{
    struct run r;
    cut_reset(&s->cd);
    if (run_mount(s, &r) != OK || r.fs.ro)
        fail("the mount");
    struct ext_fs *fs = &r.fs;
    uint64_t blks[1024];
    uint32_t nfree = free_blocks(fs, blks, 1024, 1), room = 0;
    uint32_t wtime = fs->sb.wtime;
    if (ext_jnl_begin(fs, NULL) != OK || (room = ext_jnl_room(fs)) + 1 > nfree)
        fail("room %u, %u free blocks", room, nfree);
    fs->sb.wtime = 7;
    for (uint32_t i = 0; i <= room; i++) {
        struct ext_buf *b;
        if (ext_bnew(fs, blks[i], &b) != OK)
            fail("block %u of %u", i, room);
        ext_bdirty(fs, b);
        ext_brelse(fs, b);
    }
    uint64_t units = s->cd.units;
    if (ext_jnl_room(fs) || ext_jnl_commit(fs) != ERR_NO_RESOURCES || s->cd.units != units)
        fail("a transaction of %u blocks with room for %u", room + 1, room);
    ext_jnl_abort(fs);
    if (fs->cache.ndirty || fs->sb.wtime != wtime || ext_jnl_room(fs) != room)
        fail("the abort left %u dirty, wtime %u", fs->cache.ndirty, fs->sb.wtime);
    if (ext_jnl_step(fs, room + 1) != ERR_NO_RESOURCES || ext_jnl_step(fs, room) != OK)
        fail("ext_jnl_step against the room");
    for (uint32_t i = 0; i < room; i++) {
        struct ext_buf *b;
        if (ext_bnew(fs, blks[i], &b) != OK)
            fail("block %u", i);
        ext_bdirty(fs, b);
        ext_brelse(fs, b);
    }
    if (ext_jnl_commit(fs) != OK || fs->cache.ndirty)
        fail("a transaction of exactly its room (%u)", room);
    run_free(&r);
    cut_reset(&s->cd);
}

/* Mount the device with a journal region of `size` bytes; the region's
 * revoke records. */
static uint32_t small_mount(struct storm *s, struct run *r, size_t size)
{
    struct ext_dev dev = cut_dev(&s->cd);
    memset(r, 0, sizeof(*r));
    r->cache = malloc(CACHE_BYTES);
    r->jmem = malloc(size);
    if (!r->cache || !r->jmem || ext_jnl_init(&r->j, r->jmem, size, 0) != OK ||
        ext_jnl_mount(&r->fs, &r->j, &dev, r->cache, CACHE_BYTES, 0) != OK)
        fail("a mount with a region of %zu bytes", size);
    return r->j.cap_revokes;
}

/* A replay whose revoke records don't fit its region leaves the volume
 * read-only and still needing recovery; with room, it replays. */
static void check_revoke_overflow(struct storm *s)
{
    struct run r;
    cut_reset(&s->cd);
    size_t small = 8 * (size_t)s->cd.block_size + 40000;
    uint32_t cap = small_mount(s, &r, small) + 64;   /* more than the small region holds */
    run_free(&r);
    cut_reset(&s->cd);
    if (run_mount(s, &r) != OK || r.fs.ro)
        fail("the mount");
    static uint64_t blks[4096];
    if (cap > 4096 || free_blocks(&r.fs, blks, cap, 1) != cap || ext_jnl_begin(&r.fs, NULL) != OK)
        fail("%u free blocks", cap);
    for (uint32_t i = 0; i < cap; i++)
        if (ext_jnl_revoke(&r.fs, blks[i]) != OK)
            fail("revoke %u", i);
    if (ext_jnl_commit(&r.fs) != OK)
        fail("a commit of %u revokes", cap);
    run_free(&r);
    /* The volume as the service left it when it was killed. */
    small_mount(s, &r, small);
    if (!(r.fs.ro & EXT_RO_RECOVER) || r.fs.jnl || !r.fs.why ||
        strcmp(r.fs.why, "journal: more revoke records than its memory holds"))
        fail("a replay with room for %u of %u revokes: ro %#x (%s)", r.j.cap_revokes, cap,
             r.fs.ro, r.fs.why ? r.fs.why : "");
    run_free(&r);
    if (run_mount(s, &r) != OK || r.fs.ro || r.j.replayed != 1)
        fail("the replay with room: ro %#x, replayed %u", r.fs.ro, r.j.replayed);
    run_free(&r);
    cut_reset(&s->cd);
}

/* ---- all of it ----------------------------------------------------------------------------- */

void check_journal(struct img *fixtures, const char *listdir, const char *images)
{
    /* The region for a log of 2^31 blocks is counted in 64 bits: it once
     * wrapped to a live set of 16 slots, which a writer then filled and
     * looped in (the review's R5; such a log isn't written: jnl_disk.c). */
    size_t huge = ext_jnl_mem_size(4096, 0x80000001u, 16);
    if (huge < (size_t)1 << 33)
        fail("a journal of 2^31 blocks asks for a region of %zu bytes", huge);
    for (unsigned f = 0; f < 2; f++) {
        static struct storm s;
        static const struct storm_cfg plain[2] = {
            { "jam4k-plain", 0, 0, false, 0, 0, 1, 1, 0, 0, false },
            { "linux1k-plain", 1, 0, false, 0, 0, 1, 1, 0, 0, false },
        };
        static const struct storm_cfg small[2] = {
            { "jam4k-small", 0, 0, false, 48, 0, 1, 1, 0, 0, false },
            { "linux1k-small", 1, 0, false, 64, 0, 1, 1, 0, 0, false },
        };
        where = plain[f].name;
        storm_make(&s, &plain[f], &fixtures[f]);
        check_mount_close(&s);
        check_bad_jsb(&s);
        check_revoke_overflow(&s);
        check_linux_commit(&s);
        check_freed(&s);
        cut_free(&s.cd);
        where = small[f].name;
        storm_make(&s, &small[f], &fixtures[f]);
        check_too_big(&s);
        cut_free(&s.cd);
    }
    for (unsigned i = 0; i < sizeof(storms) / sizeof(storms[0]); i++)
        check_storm(fixtures, &storms[i], listdir, images);
    check_damaged_log(fixtures, &storms[0], 200);
    check_damaged_log(fixtures, &storms[3], 200);
}
