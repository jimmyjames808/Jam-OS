/* posixtest's files section, the children:
 *
 *   - a child given three descriptors by a hand-over (a file read up to
 *     offset 4, an O_APPEND file, a directory) and a current directory:
 *     it gets its own open of each file at the parent's offset (M13 Q5),
 *     and the offsets move apart after; its appends and the parent's
 *     land in order;
 *   - a child given /data as a read-only view: every change is EROFS,
 *     mkdir of what is there EEXIST, reading works.
 *
 * Until posix_spawn (track B3) the hand-over is built here, from the
 * descriptor table's own fd_give (the layer's user/posix/sys/fd.h: this
 * file is the test's one user of the layer's insides) and sent by
 * libjam's spawn. */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include "../sys/fd.h"
#include "ptfiles.h"

#define CHILD_WAIT_NS (60 * NS_PER_S)
#define GIVE_MAX      8

/* Start this program in mode with descriptors want[i] of ours as
 * at[i] in the child, the current directory cwd, and the namespace ns;
 * its exit code, or -1. Our descriptors 1 and 2 go along as its 1 and 2
 * when they can (so its failures print). */
static int run_child(const char *mode, const int *want, const int *at, unsigned n,
                     const char *cwd, const char *const *ns)
{
    struct fd_given g[GIVE_MAX];
    struct posixfds_desc fds[GIVE_MAX];
    unsigned k = 0;
    int from[GIVE_MAX], to[GIVE_MAX];
    for (unsigned i = 0; i < n; i++)
        from[k] = want[i], to[k++] = at[i];
    from[k] = 1, to[k++] = 1;
    from[k] = 2, to[k++] = 2;
    unsigned nfds = 0;
    for (unsigned i = 0; i < k; i++) {
        if (fd_give(from[i], &g[nfds]) < 0) {
            CHECK(i >= n);   /* ours must go; 1 and 2 only if they can */
            continue;
        }
        fds[nfds] = (struct posixfds_desc){
            .fd = to[i], .kind = g[nfds].kind, .status = g[nfds].status,
            .offset = g[nfds].offset, .data = g[nfds].data, .data_len = g[nfds].data_len,
            .handles = g[nfds].handles,
        };
        nfds++;
    }
    const struct posixfds_out ho = { .state = { .umask = 022 }, .cwd = cwd, .fds = fds,
                                     .nfds = nfds };
    const char *const argv[] = { "posixtest", mode, NULL };
    struct spawn_args a = {
        .path = "bin/posixtest", .argc = 2, .argv = argv, .job = startup_handle(SR_JOB),
        .ns = ns, .handover = &ho,
    };
    handle_t proc;
    status_t st = spawn(&a, &proc);   /* the hand-over's handles are spawn's, whatever happens */
    for (unsigned i = 0; i < nfds; i++)
        free(g[i].data);
    CHECK(st == OK);
    if (st != OK)
        return -1;
    struct process_info info;
    st = spawn_wait(proc, CHILD_WAIT_NS, &info);
    CHECK(st == OK);
    jam_handle_close(proc);
    return st == OK && !info.killed ? (int)info.exit_code : -1;
}

static void write_file(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0 && write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    close(fd);
}

static void handed_files(void)
{
    write_file(PT_DIR "/hand", "0123456789");
    int f = open(PT_DIR "/hand", O_RDONLY);
    int app = open(PT_DIR "/app2", O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    int dir = open(PT_DIR, O_RDONLY | O_DIRECTORY);
    char buf[16];
    CHECK(f >= 0 && app >= 0 && dir >= 0 && read(f, buf, 4) == 4);
    CHECK(write(app, "P1", 2) == 2);
    const int want[] = { f, app, dir }, at[] = { 3, 4, 5 };
    const char *const ns[] = { "*", NULL };
    CHECK(run_child("--child-files", want, at, 3, PT_DIR "/d", ns) == 0);
    CHECK(lseek(f, 0, SEEK_CUR) == 4);   /* the child moved its own offset only */
    CHECK(read(f, buf, 2) == 2 && !memcmp(buf, "45", 2));
    CHECK(write(app, "P2", 2) == 2);
    close(f);
    close(app);
    close(dir);
    int r = open(PT_DIR "/app2", O_RDONLY);
    memset(buf, 0, sizeof(buf));
    CHECK(r >= 0 && read(r, buf, sizeof(buf)) == 6 && !strcmp(buf, "P1C1P2"));
    close(r);
}

int pt_child_files(void)
{
    char buf[16] = { 0 }, cwd[PATH_MAX];
    CHECK(read(3, buf, sizeof(buf)) == 6 && !strcmp(buf, "456789"));
    CHECK(lseek(3, 0, SEEK_CUR) == 10);
    CHECK((fcntl(4, F_GETFL) & O_APPEND) && write(4, "C1", 2) == 2);
    struct stat st;
    CHECK(fstat(5, &st) == 0 && S_ISDIR(st.st_mode));
    int fd = openat(5, "hand", O_RDONLY);
    CHECK(fd >= 0 && fd != 3 && fd != 4 && fd != 5);
    close(fd);
    CHECK(getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, PT_DIR "/d"));
    CHECK(umask(0) == 022);
    errno = 0;
    CHECK(fstat(6, &st) == -1 && errno == EBADF);
    return pt_failures() ? 1 : 0;
}

static void read_only_view(void)
{
    const char *const ns[] = { "/data:r", "/boot", NULL };
    CHECK(run_child("--child-rofs", NULL, NULL, 0, "/", ns) == 0);
}

int pt_child_rofs(void)
{
    char buf[8] = { 0 };
    int fd = open(PT_DIR "/f", O_RDONLY);
    CHECK(fd >= 0 && read(fd, buf, 5) == 5 && !strcmp(buf, "hello"));
    close(fd);
    struct stat st;
    struct statvfs v;
    errno = 0;
    CHECK(open(PT_DIR "/f", O_WRONLY) == -1 && errno == EROFS);
    CHECK(open(PT_DIR "/new", O_WRONLY | O_CREAT, 0644) == -1 && errno == EROFS);
    CHECK(mkdir(PT_DIR "/newdir", 0755) == -1 && errno == EROFS);
    CHECK(mkdir(PT_DIR "/d", 0755) == -1 && errno == EEXIST);
    CHECK(unlink(PT_DIR "/f") == -1 && errno == EROFS);
    CHECK(rmdir(PT_DIR "/d") == -1 && errno == EROFS);
    CHECK(rename(PT_DIR "/f", PT_DIR "/g") == -1 && errno == EROFS);
    CHECK(truncate(PT_DIR "/f", 0) == -1 && errno == EROFS);
    CHECK(access(PT_DIR "/f", W_OK) == -1 && errno == EROFS);
    CHECK(access(PT_DIR "/f", R_OK) == 0);
    CHECK(stat(PT_DIR "/f", &st) == 0 && (st.st_mode & 0777) == 0444);
    CHECK(statvfs(PT_DIR, &v) == 0 && (v.f_flag & ST_RDONLY));
    return pt_failures() ? 1 : 0;
}

void pt_files_children(void)
{
    handed_files();
    read_only_view();
}
