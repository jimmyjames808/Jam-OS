/* The journal's file, <prefix>/db/journal (<pkgfmt.h> has its text).
 *
 * It is replaced whole at each step, and FAT can't rename over a file, so
 * the new text goes to journal.new, is synced, and then takes the old
 * one's name: journal removed, journal.new renamed. A cut between those two
 * leaves only journal.new, whole, and it is read in the journal's place; a
 * cut before leaves the old journal, a step behind, which only repeats a
 * step. A journal.new that was cut short is never read while `journal`
 * is there, and is overwritten by the next step.
 *
 * Every step the journal names can be done again, so a reader that finds
 * the journal one step behind does the step over. */
#include "pkg.h"

status_t pk_journal_get(struct pk_env *e, struct pkg_journal *j, uint8_t **text,
                        const char **why)
{
    char path[PK_PATH];
    uint8_t *t;
    uint64_t n;
    bool from_new = false;
    pk_db_path(e, "journal", "", path);
    status_t st = pk_read(path, PKG_JOURNAL_MAX, &t, &n);
    if (st == ERR_NOT_FOUND) {
        pk_db_path(e, "journal", ".new", path);
        st = pk_read(path, PKG_JOURNAL_MAX, &t, &n);
        from_new = true;
    }
    if (st == ERR_OUT_OF_RANGE)
        st = ERR_INVALID_ARGS;
    if (st != OK) {
        *why = st == ERR_INVALID_ARGS ? "it is too big" : "it can't be read";
        return st;
    }
    st = pkg_journal_parse(t, n, j, why);
    if (st != OK) {
        free(t);
        /* A journal.new cut short with no journal beside it: the first step never
         * finished, so nothing began. */
        return from_new ? ERR_NOT_FOUND : st == ERR_NO_MEMORY ? st : ERR_INVALID_ARGS;
    }
    *text = t;
    return OK;
}

/* The journal is at step (with state: ":new", ":alone", or ""): a test may hold it
 * there (pk_env's hold). */
static void held(struct pk_env *e, const char *step, const char *state)
{
    char point[32];
    if (e->hold && !e->dead) {
        (void)pk_flush(e);   /* the point's state is on the medium before anyone stops here */
        snprintf(point, sizeof(point), "%s%s", step, state);
        e->hold(e->hold_ctx, point);
    }
}

status_t pk_journal_put(struct pk_env *e, const struct pkg_journal *j)
{
    char path[PK_PATH], fresh[PK_PATH];
    size_t cap = 4096 + (size_t)j->nbusy * (PKG_PATH_MAX + 8);
    char *buf = malloc(cap);
    if (!buf)
        return ERR_NO_MEMORY;
    size_t n = pkg_journal_write(j, buf, cap);
    const char *step = pkg_step_name(j->step);
    pk_db_path(e, "journal", "", path);
    pk_db_path(e, "journal", ".new", fresh);
    status_t st = n ? pk_mkdirs(e, path) : ERR_INVALID_ARGS;
    if (st == OK)
        st = pk_write(e, fresh, buf, n);
    if (st == OK) {
        held(e, step, ":new");
        st = pk_remove(e, path);   /* pk_move's two changes, a hold between them */
    }
    if (st == OK) {
        held(e, step, ":alone");
        st = pk_rename(e, fresh, path);
    }
    free(buf);
    st = st == OK ? pk_flush(e) : st;
    if (st == OK)
        held(e, step, "");
    return st;
}

status_t pk_journal_drop(struct pk_env *e)
{
    char path[PK_PATH];
    pk_db_path(e, "journal", ".new", path);
    status_t st = pk_remove(e, path);   /* first: a journal left alone is only a step behind */
    if (st == OK) {
        held(e, "done", ":new");
        pk_db_path(e, "journal", "", path);
        st = pk_remove(e, path);
    }
    st = st == OK ? pk_flush(e) : st;
    if (st == OK)
        held(e, "done", "");
    return st;
}
