/* What the shell gives a POSIX program (one whose list has the word
 * `posix`) in place of the native program's output pipe: its descriptors
 * 0, 1 and 2, as the POSIX hand-over (<posixfds.h>) carries them to the
 * program's C library.
 *
 * Each of the three is one of: the terminal (console.open_tty on the
 * program's own console channel: libc's terminal descriptor); the pipe
 * that carries the previous stage's output in (the shell fills it before
 * the program starts, then closes its end: end of file after the last
 * byte); the previous program's own pipe, its read end passed on as it is
 * (sh_stdio.direct_in: a pipeline of programs runs at once); the pipe the
 * next stage reads (the shell keeps the read end and copies what comes,
 * sh_program.c, or passes it to the next program: sh_stdio.direct_out);
 * or a file a redirection names. A pipe between programs holds 1 MiB (a
 * writer waits for the reader). The
 * redirections, left to right as a Unix shell applies them, change which:
 * `< f`, `> f` (made empty), `>> f` (appended), and `2>&1` (the second
 * descriptor becomes another of the first's).
 *
 * Descriptors that are the same thing share one open file description, as
 * dup2 makes them (POSIXFDS_DUP: the later record names the earlier): the
 * terminal for all three, or `> f 2>&1`'s file with one offset for both
 * writers, as on Unix.
 *
 * A file is opened here, once, and the open itself goes to the program
 * (file_give: its file channel and buffer), so the program holds the
 * file's only open and the shell none: fat lets one open write a file, and
 * a shell that kept its own open could never hand a writable one on.
 *
 * The hand-over also carries the shell's current directory, a umask of
 * 022 and the shell's pid as the parent's. */
#include <idl/console.h>
#include <posixfds.h>
#include "sh_core.h"

/* posixfds_fd.status: Linux's O_* values (<posixfds.h>). */
#define STATUS_RDONLY 0u
#define STATUS_WRONLY 1u
#define STATUS_RDWR   2u

/* The rights a pipe's two ends have in the program (the POSIX layer's own:
 * user/posix/sys/pipe.c): it may pass them on, never widen them. */
#define PIPE_READ_RIGHTS  (RIGHT_READ | RIGHT_WAIT | RIGHT_TRANSFER | RIGHT_DUPLICATE)
#define PIPE_WRITE_RIGHTS (RIGHT_WRITE | RIGHT_WAIT | RIGHT_TRANSFER | RIGHT_DUPLICATE)

#define TTY_WAIT_NS (5 * NS_PER_S)

/* What a descriptor is, before anything is opened. */
enum spec_kind { SPEC_TERM, SPEC_PIPE_IN, SPEC_PIPE_DIRECT_IN, SPEC_PIPE_OUT, SPEC_FILE };
struct spec {
    uint8_t kind;      /* enum spec_kind */
    bool    write;     /* SPEC_FILE: opened for writing (else for reading) */
    bool    truncate;  /* SPEC_FILE, write: `>` rather than `>>` */
    char   *path;      /* SPEC_FILE: the absolute path (points into the state) */
};

/* A source: one open thing that one or more descriptors are. */
struct source {
    struct spec spec;
    handle_t    h[2];       /* the handles for the program; moved into the first descriptor */
    unsigned    nh;         /* how many of h */
    int         first_fd;   /* the first descriptor that is this source, -1 before one */
};

/* The state of one hand-over: static, since the shell starts one program
 * at a time (and the records' data, the paths, must outlive the call that
 * builds it, until spawn writes it). */
static struct {
    struct posixfds_desc desc[3];
    handle_t             hs[3][POSIXFDS_KIND_HANDLES];
    uint32_t             dup_of[3];   /* a DUP record's data */
    char                 file[SH_MAX_REDIR][SH_PATH_MAX];   /* the paths of the redirections */
    char                 cwd[SH_PATH_MAX];
    struct source        src[3];
    unsigned             nsrc;
} st;

/* The redirections of the command about to run (sh_exec.c). */
static const struct sh_redir *pending;
static int npending;

void sh_redir_set(const struct sh_redir *r, int n)
{
    pending = n ? r : NULL;
    npending = n;
}

int sh_redir_count(void)
{
    return npending;
}

/* ---- opening the sources --------------------------------------------------------------- */

/* A channel pair: *keep narrowed to keep_rights, *give to give_rights
 * (RIGHT_SAME: as made). bound: a writer that finds 1 MiB queued waits
 * (the POSIX layer's own pipes are made so, user/posix/sys/pipe.c: a
 * reader that lags would else fill the writer's job's message budget). */
static status_t pipe_pair(handle_t *keep, handle_t *give, rights_t keep_rights,
                          rights_t give_rights, bool bound)
{
    handle_t h[2];
    const rights_t rights[2] = { keep_rights, give_rights };
    status_t s = jam_channel_create(&h[0], &h[1]);
    if (s != OK)
        return s;
    /* The flag first, while each end is whole and its only handle. */
    for (int i = 0; i < 2 && bound && s == OK; i++)
        s = jam_channel_set_flags(h[i], CHANNEL_BYTE_BOUND);
    for (int i = 0; i < 2 && s == OK; i++) {
        handle_t n;
        if (rights[i] == RIGHT_SAME)
            continue;
        if ((s = jam_handle_replace(h[i], rights[i], &n)) == OK)
            h[i] = n;
        else
            h[i] = HANDLE_INVALID;   /* the replace closed it */
    }
    if (s != OK) {
        for (int i = 0; i < 2; i++)
            if (h[i] != HANDLE_INVALID)
                jam_handle_close(h[i]);
        return s;
    }
    *keep = h[0];
    *give = h[1];
    return OK;
}

/* The previous stage's output as a pipe for the program's input: all of
 * it queued (a pipe holds at most SH_PIPE_MAX bytes), our end closed. */
static status_t open_pipe_in(struct source *sc)
{
    handle_t ours;
    status_t s = pipe_pair(&ours, &sc->h[0], RIGHT_SAME, PIPE_READ_RIGHTS, false);
    if (s != OK)
        return s;
    struct sh_stdio io = sh_stdio_get();
    for (size_t at = 0; s == OK && at < io.in_len; at += CHANNEL_MAX_BYTES) {
        size_t n = io.in_len - at < CHANNEL_MAX_BYTES ? io.in_len - at : CHANNEL_MAX_BYTES;
        s = jam_channel_write(ours, io.in + at, (uint32_t)n, NULL, 0);
    }
    jam_handle_close(ours);
    if (s != OK)
        jam_handle_close(sc->h[0]);
    else
        sc->nh = 1;
    return s;
}

static status_t open_file(struct source *sc)
{
    uint32_t flags = sc->spec.write ? FS_WRITE | FS_CREATE | (sc->spec.truncate ? FS_TRUNCATE : 0)
                                    : FS_READ;
    struct jfile jf;
    status_t s = file_open(sc->spec.path, flags, &jf);
    if (s != OK) {
        sh_tty("sh: %s: %s\n", sc->spec.path, sh_why(s));
        return s;
    }
    file_give(&jf, &sc->h[0], &sc->h[1]);
    sc->nh = 2;
    return OK;
}

/* Open source sc (sc->nh is what it holds afterwards); *out_r gets the
 * shell's end of an output pipe. console: the program's console channel,
 * for its terminal. */
static status_t open_source(struct source *sc, handle_t console, handle_t *out_r)
{
    status_t s;
    switch (sc->spec.kind) {
    case SPEC_TERM:
        s = console_open_tty_until(console, now() + TTY_WAIT_NS, &sc->h[0], &sc->h[1]);
        sc->nh = s == OK ? 2 : 0;
        return s;
    case SPEC_PIPE_OUT:
        s = pipe_pair(out_r, &sc->h[0], PIPE_READ_RIGHTS, PIPE_WRITE_RIGHTS, true);
        sc->nh = s == OK ? 1 : 0;
        return s;
    case SPEC_PIPE_DIRECT_IN: {
        struct sh_stdio io = sh_stdio_get();
        sc->h[0] = io.direct_in;   /* the program's now */
        sc->nh = 1;
        io.direct_in = HANDLE_INVALID;
        sh_stdio_set(io);
        return OK;
    }
    case SPEC_PIPE_IN:
        return open_pipe_in(sc);
    default:
        return open_file(sc);
    }
}

static void discard_sources(void)
{
    for (unsigned k = 0; k < st.nsrc; k++)
        for (unsigned i = 0; i < st.src[k].nh; i++)
            jam_handle_close(st.src[k].h[i]);
}

/* ---- from the redirections to the descriptors ------------------------------------------- */

static bool same_spec(const struct spec *a, const struct spec *b)
{
    if (a->kind != b->kind)
        return false;
    return a->kind != SPEC_FILE || (a->write == b->write && !strcmp(a->path, b->path));
}

/* The specs of descriptors 0 to 2 after the redirections; false (said) if
 * a path is too long to make sense of. */
static bool apply_redirections(struct spec spec[3], bool have_in, bool direct_in, bool piped)
{
    spec[0] = (struct spec){ .kind = direct_in ? SPEC_PIPE_DIRECT_IN
                                   : have_in   ? SPEC_PIPE_IN : SPEC_TERM };
    spec[1] = (struct spec){ .kind = piped ? SPEC_PIPE_OUT : SPEC_TERM };
    spec[2] = (struct spec){ .kind = SPEC_TERM };
    unsigned used = 0;   /* st.file[] slots taken */
    for (int i = 0; i < npending; i++) {
        const struct sh_redir *r = &pending[i];
        if (r->kind == SH_REDIR_DUP) {
            spec[r->fd] = spec[r->from];
            continue;
        }
        if (!sh_resolve(r->path, st.file[used], sizeof(st.file[used]))) {
            sh_tty("sh: %s: the name is too long\n", r->path);
            return false;
        }
        spec[r->fd] = (struct spec){
            .kind = SPEC_FILE, .write = r->kind != SH_REDIR_IN,
            .truncate = r->kind == SH_REDIR_OUT, .path = st.file[used++],
        };
    }
    return true;
}

/* The distinct sources the three specs make, into st.src; of[fd] is the
 * one descriptor fd is. */
static void gather_sources(const struct spec spec[3], unsigned of[3])
{
    st.nsrc = 0;
    for (unsigned fd = 0; fd < 3; fd++) {
        unsigned k = 0;
        while (k < st.nsrc && !same_spec(&st.src[k].spec, &spec[fd]))
            k++;
        if (k == st.nsrc)
            st.src[st.nsrc++] = (struct source){ .spec = spec[fd], .first_fd = -1 };
        else if (spec[fd].truncate)
            st.src[k].spec.truncate = true;   /* `> f 2>> f`: made empty, and both write on */
        of[fd] = k;
    }
}

/* The record for descriptor fd, a source's: the first one to use it takes
 * its handles, a later one is a DUP of that. */
static void describe(unsigned fd, struct source *sc)
{
    struct posixfds_desc *d = &st.desc[fd];
    if (sc->first_fd >= 0) {
        st.dup_of[fd] = (uint32_t)sc->first_fd;
        *d = (struct posixfds_desc){ .fd = (int32_t)fd, .kind = POSIXFDS_DUP,
                                     .data = &st.dup_of[fd], .data_len = sizeof(uint32_t) };
        return;
    }
    sc->first_fd = (int)fd;
    memcpy(st.hs[fd], sc->h, sc->nh * sizeof(handle_t));
    *d = (struct posixfds_desc){ .fd = (int32_t)fd, .handles = st.hs[fd] };
    switch (sc->spec.kind) {
    case SPEC_TERM:
        d->kind = POSIXFDS_TTY;
        d->status = STATUS_RDWR;
        break;
    case SPEC_PIPE_IN:
    case SPEC_PIPE_DIRECT_IN:
        d->kind = POSIXFDS_PIPE;
        d->status = STATUS_RDONLY;
        break;
    case SPEC_PIPE_OUT:
        d->kind = POSIXFDS_PIPE;
        d->status = STATUS_WRONLY;
        break;
    default:
        d->kind = POSIXFDS_FILE;
        d->status = !sc->spec.write ? STATUS_RDONLY
                    : sc->spec.truncate ? STATUS_WRONLY : STATUS_WRONLY | POSIXFDS_O_APPEND;
        d->data = sc->spec.path;
        d->data_len = (uint32_t)strlen(sc->spec.path) + 1;
    }
}

bool sh_posix_prepare(struct posixfds_out *out, handle_t console, handle_t *out_r)
{
    struct spec spec[3];
    unsigned of[3];
    struct sh_stdio io = sh_stdio_get();
    if (!apply_redirections(spec, io.have_in, io.direct_in != HANDLE_INVALID, sh_piped()))
        return false;
    gather_sources(spec, of);
    for (unsigned k = 0; k < st.nsrc; k++) {
        if (open_source(&st.src[k], console, out_r) == OK)
            continue;
        if (st.src[k].spec.kind != SPEC_FILE)
            sh_tty("sh: can't set up the program's %s\n",
                   st.src[k].spec.kind == SPEC_TERM ? "terminal" : "pipe");
        discard_sources();
        return false;
    }
    for (unsigned fd = 0; fd < 3; fd++)
        describe(fd, &st.src[of[fd]]);   /* the handles are the records' now */
    struct process_info me = { 0 };
    (void)jam_process_get_info(startup_handle(SR_SELF_PROCESS), &me);
    snprintf(st.cwd, sizeof(st.cwd), "%s", sh_cwd());
    *out = (struct posixfds_out){
        .state = { .umask = 022, .ppid = me.pid },
        .cwd = st.cwd, .fds = st.desc, .nfds = 3,
    };
    return true;
}
