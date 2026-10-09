/* posixtest's fds section: the descriptor kinds libc makes itself, and
 * the calls on descriptors (docs/M13-PLAN.md sections 3 and 11).
 *
 *   - dup, dup2, dup3 and fcntl: a dup shares the offset and the status
 *     flags, FD_CLOEXEC is the slot's own; F_DUPFD's lowest free number;
 *     the locks accepted; the errors;
 *   - memfd: writes, reads at an offset, ftruncate up (zeros) and down,
 *     then up again (zeros, not the old bytes), the seals;
 *   - eventfd: the counter, the semaphore mode, EAGAIN, a read woken by
 *     another thread's write;
 *   - the one user's ids;
 *   - /dev and /etc through a hand-over (a child given /dev/zero,
 *     /dev/full, /dev/urandom and /etc/passwd as descriptors), and through
 *     open, getpwnam and shm_open once open is built. */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <jamos/libjam.h>
#include <posixfds.h>
#include <pthread.h>
#include <pwd.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "pt.h"

#define CHILD_WAIT_NS (30 * NS_PER_S)

static void check_dup(void)
{
    int fd = memfd_create("dup", 0);
    CHECK(fd > 2);
    CHECK(write(fd, "abcdef", 6) == 6);
    int d = dup(fd);
    CHECK(d > fd);
    CHECK(lseek(d, 1, SEEK_SET) == 1);
    char c;
    CHECK(read(fd, &c, 1) == 1 && c == 'b');   /* one offset */
    CHECK(fcntl(d, F_SETFL, O_NONBLOCK) == 0 && (fcntl(fd, F_GETFL) & O_NONBLOCK));
    CHECK(fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 && fcntl(fd, F_GETFD) == FD_CLOEXEC &&
          fcntl(d, F_GETFD) == 0);   /* the slot's own */
    int high = fcntl(fd, F_DUPFD, 100);
    CHECK(high == 100 && fcntl(high, F_GETFD) == 0);
    int high2 = fcntl(fd, F_DUPFD_CLOEXEC, 100);
    CHECK(high2 == 101 && fcntl(high2, F_GETFD) == FD_CLOEXEC);
    CHECK(dup2(fd, 200) == 200 && dup2(200, 200) == 200);
    CHECK(dup3(fd, 201, O_CLOEXEC) == 201 && fcntl(201, F_GETFD) == FD_CLOEXEC);
    errno = 0;
    CHECK(dup3(fd, fd, 0) == -1 && errno == EINVAL);
    CHECK(dup3(fd, 202, O_NONBLOCK) == -1 && errno == EINVAL);
    CHECK(dup(999) == -1 && errno == EBADF);
    CHECK(dup2(999, 5) == -1 && errno == EBADF);
    CHECK(fcntl(fd, F_DUPFD, -1) == -1 && errno == EINVAL);
    CHECK(fcntl(fd, F_SETFL, O_ASYNC) == -1 && errno == EINVAL);
    CHECK(fcntl(fd, F_GETPIPE_SZ) == -1 && errno == EBADF);
    struct flock l = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
    CHECK(fcntl(fd, F_SETLK, &l) == 0 && fcntl(fd, F_SETLKW, &l) == 0);
    CHECK(fcntl(fd, F_GETLK, &l) == 0 && l.l_type == F_UNLCK);
    CHECK(lockf(fd, F_LOCK, 0) == 0);
    for (int x = 0; x < 5; x++)
        close((int[]){ fd, d, high, high2, 200 }[x]);
    close(201);
    CHECK(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
}

static void check_memfd(void)
{
    int fd = memfd_create("pool", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    CHECK(fd > 2 && fcntl(fd, F_GETFD) == FD_CLOEXEC);
    char buf[64];
    CHECK(write(fd, "hello", 5) == 5);
    CHECK(pread(fd, buf, sizeof(buf), 1) == 4 && !memcmp(buf, "ello", 4));
    CHECK(ftruncate(fd, 10000) == 0);
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 10000);
    CHECK(pread(fd, buf, 8, 4996) == 8 && !memcmp(buf, "\0\0\0\0\0\0\0\0", 8));
    CHECK(pwrite(fd, "tail", 4, 9000) == 4);
    CHECK(ftruncate(fd, 2) == 0 && fstat(fd, &st) == 0 && st.st_size == 2);
    CHECK(pread(fd, buf, sizeof(buf), 0) == 2 && !memcmp(buf, "he", 2));
    CHECK(ftruncate(fd, 9100) == 0);
    CHECK(pread(fd, buf, 4, 9000) == 4 && !memcmp(buf, "\0\0\0\0", 4));   /* not "tail" */
    CHECK(pread(fd, buf, 3, 2) == 3 && !memcmp(buf, "\0\0\0", 3));        /* not "llo" */
    CHECK(lseek(fd, 0, SEEK_END) == 9100);
    CHECK(fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK) == 0);
    CHECK(fcntl(fd, F_GET_SEALS) == F_SEAL_SHRINK);
    errno = 0;
    CHECK(ftruncate(fd, 1) == -1 && errno == EPERM);
    CHECK(ftruncate(fd, 9200) == 0);
    CHECK(fcntl(fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SEAL) == 0);
    CHECK(pwrite(fd, "x", 1, 9200) == -1 && errno == EPERM);
    CHECK(fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE) == -1 && errno == EPERM);
    close(fd);
    fd = memfd_create("plain", 0);
    CHECK(fcntl(fd, F_ADD_SEALS, F_SEAL_GROW) == -1 && errno == EPERM);   /* no sealing */
    close(fd);
    CHECK(memfd_create("x", MFD_HUGETLB) == -1 && errno == EINVAL);
}

struct efd_waker {
    int fd;
};

static void *efd_wake(void *arg)
{
    struct timespec nap = { 0, 50 * 1000 * 1000 };
    nanosleep(&nap, NULL);
    CHECK(eventfd_write(((struct efd_waker *)arg)->fd, 42) == 0);
    return NULL;
}

static void check_eventfd(void)
{
    int fd = eventfd(5, EFD_NONBLOCK);
    eventfd_t v = 0;
    CHECK(fd > 2);
    CHECK(eventfd_write(fd, 3) == 0 && eventfd_read(fd, &v) == 0 && v == 8);
    errno = 0;
    CHECK(eventfd_read(fd, &v) == -1 && errno == EAGAIN);
    uint64_t max = UINT64_MAX;
    CHECK(write(fd, &max, sizeof(max)) == -1 && errno == EINVAL);
    char small[4];
    CHECK(read(fd, small, sizeof(small)) == -1 && errno == EINVAL);
    max = UINT64_MAX - 1;
    CHECK(write(fd, &max, sizeof(max)) == 8);
    CHECK(eventfd_write(fd, 1) == -1 && errno == EAGAIN);   /* no room */
    CHECK(eventfd_read(fd, &v) == 0 && v == UINT64_MAX - 1);
    close(fd);
    fd = eventfd(2, EFD_SEMAPHORE);
    CHECK(eventfd_read(fd, &v) == 0 && v == 1 && eventfd_read(fd, &v) == 0 && v == 1);
    struct efd_waker w = { fd };
    pthread_t t;
    CHECK(pthread_create(&t, NULL, efd_wake, &w) == 0);
    CHECK(eventfd_read(fd, &v) == 0 && v == 1);   /* waited for the write */
    CHECK(pthread_join(t, NULL) == 0);
    close(fd);
    CHECK(eventfd(0, 0x1234) == -1 && errno == EINVAL);
}

static void check_user(void)
{
    CHECK(getuid() == 1000 && geteuid() == 1000 && getgid() == 1000 && getegid() == 1000);
    uid_t r, e, s;
    CHECK(getresuid(&r, &e, &s) == 0 && r == 1000 && e == 1000 && s == 1000);
    CHECK(setuid(1000) == 0 && setgid(1000) == 0 && seteuid(1000) == 0);
    errno = 0;
    CHECK(setuid(0) == -1 && errno == EPERM);
    CHECK(setgid(0) == -1 && errno == EPERM);
    CHECK(setgroups(0, NULL) == -1 && errno == EPERM);
    gid_t g[4];
    CHECK(getgroups(0, NULL) == 1 && getgroups(4, g) == 1 && g[0] == 1000);
}

/* ---- /dev and /etc ------------------------------------------------------- */

int pt_child_devetc(void)
{
    char buf[256];
    memset(buf, 1, sizeof(buf));
    CHECK(read(3, buf, 100) == 100 && !buf[0] && !buf[99]);   /* /dev/zero */
    errno = 0;
    CHECK(write(4, "x", 1) == -1 && errno == ENOSPC);          /* /dev/full */
    unsigned char r[32] = { 0 }, or = 0;
    CHECK(read(5, r, sizeof(r)) == sizeof(r));                 /* /dev/urandom */
    for (unsigned i = 0; i < sizeof(r); i++)
        or |= r[i];
    CHECK(or != 0);
    ssize_t n = read(6, buf, sizeof(buf) - 1);                 /* /etc/passwd */
    CHECK(n > 0);
    buf[n > 0 ? n : 0] = '\0';
    CHECK(!strncmp(buf, "jam:x:1000:1000:", 16));
    struct stat st;
    CHECK(fstat(3, &st) == 0 && S_ISCHR(st.st_mode));
    CHECK(fstat(6, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == n);
    return pt_failures() ? 1 : 0;
}

static void check_devetc_handover(void)
{
    const struct posixfds_desc fds[] = {
        { .fd = 3, .kind = POSIXFDS_DEV, .status = 0, .data = "zero", .data_len = 5 },
        { .fd = 4, .kind = POSIXFDS_DEV, .status = 1, .data = "full", .data_len = 5 },
        { .fd = 5, .kind = POSIXFDS_DEV, .status = 0, .data = "urandom", .data_len = 8 },
        { .fd = 6, .kind = POSIXFDS_ETC, .status = 0, .data = "/etc/passwd", .data_len = 12 },
    };
    const struct posixfds_out ho = { .cwd = "/", .fds = fds, .nfds = 4 };
    const char *const argv[] = { "posixtest", "--child-devetc", NULL };
    struct spawn_args a = {
        .path = "bin/posixtest", .argc = 2, .argv = argv, .job = startup_handle(SR_JOB),
        .handover = &ho,
    };
    handle_t proc;
    status_t st = spawn(&a, &proc);
    CHECK(st == OK);
    if (st != OK)
        return;
    struct process_info info;
    CHECK(spawn_wait(proc, CHILD_WAIT_NS, &info) == OK && !info.killed && info.exit_code == 0);
    jam_handle_close(proc);
}

/* Through the path calls: run only once open reaches the descriptor
 * table's paths (ENOSYS before). */
static void check_devetc_paths(void)
{
    int fd = open("/dev/null", O_RDWR);
    if (fd < 0 && errno == ENOSYS) {
        printf("posixtest: fds: open() not built yet: /dev and /etc paths not checked\n");
        return;
    }
    char c;
    CHECK(fd > 2 && write(fd, "x", 1) == 1 && read(fd, &c, 1) == 0);
    close(fd);
    struct passwd *pw = getpwnam("jam");
    CHECK(pw && pw->pw_uid == 1000 && !strcmp(pw->pw_dir, "/data"));
    CHECK(getpwuid(1000) != NULL && getgrgid(1000) != NULL);
    errno = 0;
    CHECK(open("/etc/passwd", O_WRONLY) == -1 && errno == EROFS);
    CHECK(open("/dev/nothing", O_RDONLY) == -1 && errno == ENOENT);
    fd = shm_open("/ptshm", O_RDWR | O_CREAT | O_EXCL, 0600);
    CHECK(fd > 2 && ftruncate(fd, 4096) == 0 && write(fd, "shm", 3) == 3);
    int again = shm_open("/ptshm", O_RDONLY, 0);
    char buf[4] = { 0 };
    CHECK(again > 2 && read(again, buf, 3) == 3 && !strcmp(buf, "shm"));
    CHECK(shm_open("/ptshm", O_RDWR | O_CREAT | O_EXCL, 0600) == -1 && errno == EEXIST);
    CHECK(shm_unlink("/ptshm") == 0 && shm_open("/ptshm", O_RDWR, 0) == -1 && errno == ENOENT);
    close(fd);
    close(again);
    fd = open("/dev/stdout", O_WRONLY);
    CHECK(fd > 2);
    close(fd);
}

void pt_fds(char **argv __attribute__((unused)))
{
    check_dup();
    check_memfd();
    check_eventfd();
    check_user();
    check_devetc_handover();
    check_devetc_paths();
}
