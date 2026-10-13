/* The wayland section's rig: a client Wayland socket and a fake
 * compositor end (wlrig.h has the model). */
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "pt.h"
#include "wlrig.h"

uint64_t wr_job_handles(void)
{
    struct job_info ji;
    return jam_job_get_info(startup_handle(SR_JOB), &ji) == OK ? ji.used[JOB_LIMIT_HANDLES] : 0;
}

uint32_t wr_rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

void wr_wr32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, 4);
}

void wr_msg(uint8_t *p, uint32_t id, uint32_t size)
{
    wr_wr32(p, id);
    wr_wr32(p + 4, size << 16 | 1);
    for (uint32_t i = 8; i < size; i++)
        p[i] = (uint8_t)(i * 7 + id);
}

int wr_sock_new(void)
{
    return socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
}

int wr_connect_to(int fd, const char *path)
{
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
    return connect(fd, (struct sockaddr *)&a, sizeof(a));
}

bool wr_open(struct rig *r)
{
    handle_t a, b;
    if (jam_channel_create(&a, &b) != OK)
        return false;
    if (ns_svc_set(SVC_WAYLAND, a, false) != OK) {   /* consumes a */
        jam_handle_close(b);
        return false;
    }
    r->comp = b;
    r->fd = wr_sock_new();
    bool ok = r->fd >= 0 && wr_connect_to(r->fd, "/svc/wayland") == 0;
    (void)ns_svc_remove(SVC_WAYLAND);   /* the client's end is its only handle now */
    if (!ok) {
        if (r->fd >= 0)
            close(r->fd);
        jam_handle_close(b);
    }
    return ok;
}

void wr_close(struct rig *r)
{
    close(r->fd);
    jam_handle_close(r->comp);
}

status_t wr_comp_read(struct rig *r, struct batch *bt)
{
    struct channel_read_args a = {
        .h = r->comp, .rd_cap = sizeof(bt->b), .rd_bytes = (uint64_t)(uintptr_t)bt->b,
        .rd_actual = (uint64_t)(uintptr_t)&bt->nb, .rd_handles = (uint64_t)(uintptr_t)bt->h,
        .rd_hcap = JWL_BATCH_HANDLES, .rd_hactual = (uint64_t)(uintptr_t)&bt->nh,
    };
    bt->nb = bt->nh = 0;
    return jam_channel_read(&a);
}

bool wr_batch_ok(const struct batch *bt)
{
    if (bt->nb < JWL_HEADER_BYTES || bt->nb > JWL_BATCH_MAX || wr_rd32(bt->b) != JWL_MAGIC ||
        wr_rd32(bt->b + 4) != bt->nh || wr_rd32(bt->b + 8) || wr_rd32(bt->b + 12))
        return false;
    for (uint32_t at = JWL_HEADER_BYTES; at < bt->nb;) {
        uint32_t size = bt->nb - at >= 8 ? wr_rd32(bt->b + at + 4) >> 16 : 0;
        if (size < 8 || size > JWL_MSG_MAX || size % 4 || size > bt->nb - at)
            return false;
        at += size;
    }
    return true;
}

void wr_close_handles(const handle_t *h, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        jam_handle_close(h[i]);
}

status_t wr_comp_write_hdr(struct rig *r, const uint32_t hdr[2], const void *bytes, uint32_t n,
                           handle_t *h, uint32_t nh)
{
    static uint8_t b[JWL_BATCH_MAX];   /* one thread: the section's */
    wr_wr32(b, hdr[0]);
    wr_wr32(b + 4, hdr[1]);
    wr_wr32(b + 8, 0);
    wr_wr32(b + 12, 0);
    memcpy(b + JWL_HEADER_BYTES, bytes, n);
    return jam_channel_write(r->comp, b, JWL_HEADER_BYTES + n, h, nh);
}

status_t wr_comp_write(struct rig *r, const void *bytes, uint32_t n, handle_t *h, uint32_t nh)
{
    const uint32_t hdr[2] = { JWL_MAGIC, nh };
    return wr_comp_write_hdr(r, hdr, bytes, n, h, nh);
}

ssize_t wr_send(int fd, const void *p, size_t n, const int *fds, unsigned nfds)
{
    char ctl[CMSG_SPACE(40 * sizeof(int))];
    struct iovec v = { (void *)p, n };
    struct msghdr m = { .msg_iov = &v, .msg_iovlen = 1 };
    if (nfds) {
        m.msg_control = ctl;
        m.msg_controllen = CMSG_SPACE(nfds * sizeof(int));
        struct cmsghdr *c = CMSG_FIRSTHDR(&m);
        c->cmsg_len = CMSG_LEN(nfds * sizeof(int));
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        memcpy(CMSG_DATA(c), fds, nfds * sizeof(int));
    }
    return sendmsg(fd, &m, MSG_NOSIGNAL | MSG_DONTWAIT);
}

ssize_t wr_recv(int fd, void *buf, size_t n, unsigned room, struct rx *out)
{
    char ctl[CMSG_SPACE(JWL_HANDLES_QUEUED * sizeof(int))];
    struct iovec v = { buf, n };
    struct msghdr m = { .msg_iov = &v, .msg_iovlen = 1, .msg_control = room ? ctl : NULL,
                        .msg_controllen = room ? CMSG_SPACE(room * sizeof(int)) : 0 };
    ssize_t got = recvmsg(fd, &m, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    out->n = 0;
    out->flags = m.msg_flags;
    struct cmsghdr *c = got >= 0 ? CMSG_FIRSTHDR(&m) : NULL;
    if (c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
        out->n = (unsigned)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        memcpy(out->fds, CMSG_DATA(c), out->n * sizeof(int));
    }
    return got;
}

int wr_memfd_with(const char *text)
{
    int fd = memfd_create("wl-test", MFD_CLOEXEC);
    CHECK(fd >= 0 && write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    return fd;
}

handle_t wr_vmo_with(const char *text, rights_t rights)
{
    handle_t v = HANDLE_INVALID;
    CHECK(jam_vmo_create(4096, VMO_KEEP_PAGES, &v) == OK);
    CHECK(jam_vmo_write(v, 0, text, strlen(text)) == OK);
    if (rights != RIGHT_SAME)
        CHECK(jam_handle_replace(v, rights, &v) == OK);
    return v;
}
