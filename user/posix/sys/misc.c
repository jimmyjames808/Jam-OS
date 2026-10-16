/* The machine and randomness: uname, sysinfo and sched_getaffinity (what
 * musl's sysconf reads for the CPUs and the memory) from the figures any
 * process may read (sys_public_info, kposix.c; without it: one CPU, no
 * memory figure, an "unknown" Jam OS version), and getrandom from the kernel's
 * generator (random_get, through libjam's os_random).
 *
 * sched_setaffinity is accepted and changes nothing: every thread runs
 * wherever the scheduler puts it, which is every CPU (a mask with none of
 * the CPUs in it is EINVAL, as on Linux). membarrier is ENOSYS, as an old
 * kernel answers: musl then does the barrier itself with signals, and a
 * program that asks which commands exist is told none. */
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/random.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include "layer.h"
#include "threads.h"

#define SYSNAME       "JamOS"   /* one word: scripts match `uname -s` */
#define NODENAME      "jamos"   /* the host's name until the setting is read */
#define MACHINE       "x86_64"
#define GETRANDOM_MAX 33554431  /* Linux's most bytes one getrandom gives */
#define AFFINITY_MAX  128       /* bytes of CPU mask: 1024 CPUs */

static void copy_field(char *dst, size_t cap, const char *src)
{
    size_t n = strnlen(src, cap - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* uname's release is a Linux kernel release, not Jam OS's version
 * (docs/CR-PLAN.md, "CBF2b notes"): programs of the Linux ABI read it as
 * "major.minor.patch" and gate features on it (Chromium's
 * SysInfo::KernelVersionNumber, libuv's io_uring, rand_util's getrandom),
 * and "0.1.0" put Jam OS below every gate. 5.10 is Debian 11's kernel, the
 * one Chromium's own Linux sysroot is built for: the calls the layer
 * answers are the ones a 5.10 has, a feature newer than that is tried only
 * by a program that probes for it (ENOSYS). The suffix keeps it plain that
 * this is not a Linux; Jam OS's own version is uname's `version`. */
#define LX_RELEASE    "5.10.0-jamos"

long lx_uname(long out, long b, long c, long d, long e, long f)
{
    struct utsname u;
    struct kp_public pub;
    char version[sizeof(u.version)];
    snprintf(version, sizeof(version), "#1 JamOS %s",
             kp_public_info(&pub) == OK && pub.version[0] ? pub.version : "unknown");
    memset(&u, 0, sizeof(u));
    copy_field(u.sysname, sizeof(u.sysname), SYSNAME);
    copy_field(u.nodename, sizeof(u.nodename), NODENAME);
    copy_field(u.release, sizeof(u.release), LX_RELEASE);
    copy_field(u.version, sizeof(u.version), version);
    copy_field(u.machine, sizeof(u.machine), MACHINE);
    copy_field(u.domainname, sizeof(u.domainname), "(none)");
    return uc_put((void *)(uintptr_t)out, &u, sizeof(u));
}

long lx_sysinfo(long out, long b, long c, long d, long e, long f)
{
    struct sysinfo s;
    struct kp_public pub;
    memset(&s, 0, sizeof(s));
    s.uptime = (long)(now() / NS_PER_S);
    s.procs = 1;
    s.mem_unit = 1;
    if (kp_public_info(&pub) == OK) {
        s.totalram = pub.pages * pub.page_size;
        s.freeram = pub.free_pages * pub.page_size;
    }
    return uc_put((void *)(uintptr_t)out, &s, sizeof(s));
}

static status_t thread_exists(handle_t t, void *arg)
{
    return OK;
}

/* CPUs online. */
static unsigned cpus_online(void)
{
    struct kp_public pub;
    return kp_public_info(&pub) == OK && pub.cpus ? pub.cpus : 1;
}

long lx_sched_getaffinity(long pid, long size, long mask, long d, long e, long f)
{
    if (size <= 0 || size % (long)sizeof(long))
        return -EINVAL;
    long r = thread_apply((int)pid, thread_exists, NULL);
    if (r)
        return r;
    unsigned cpus = cpus_online();
    size_t n = size < AFFINITY_MAX ? (size_t)size : AFFINITY_MAX;
    if (cpus > n * 8)
        cpus = (unsigned)(n * 8);
    uint8_t m[AFFINITY_MAX];
    memset(m, 0, n);
    for (unsigned i = 0; i < cpus; i++)
        m[i / 8] |= (uint8_t)(1u << (i % 8));
    return uc_put((void *)(uintptr_t)mask, m, n) ? -EFAULT : (long)n;
}

long lx_sched_setaffinity(long pid, long size, long mask, long d, long e, long f)
{
    /* Linux reads the mask first (a missing one is a bad address); it is
     * zero-extended past size, and only the CPUs that exist count. */
    uint8_t m[AFFINITY_MAX];
    size_t n = size < AFFINITY_MAX ? (size_t)size : AFFINITY_MAX;
    if (size < 0 || !mask || uc_get(m, (const void *)(uintptr_t)mask, n))
        return -EFAULT;
    long r = thread_apply((int)pid, thread_exists, NULL);
    if (r)
        return r;
    unsigned cpus = cpus_online();
    for (unsigned i = 0; i < cpus && i / 8 < n; i++)
        if (m[i / 8] & (1u << (i % 8)))
            return 0;
    return -EINVAL;
}

long lx_membarrier(long cmd, long flags, long c, long d, long e, long f)
{
    return -ENOSYS;
}

long lx_getrandom(long buf, long len, long flags, long d, long e, long f)
{
    if (flags & ~(long)(GRND_NONBLOCK | GRND_RANDOM | GRND_INSECURE))
        return -EINVAL;
    if (len < 0)
        return -EINVAL;
    size_t n = len > GETRANDOM_MAX ? GETRANDOM_MAX : (size_t)len;
    uint8_t chunk[512];   /* made here, then copied to the caller's */
    for (size_t at = 0; at < n; at += sizeof(chunk)) {
        size_t k = n - at < sizeof(chunk) ? n - at : sizeof(chunk);
        os_random(chunk, k);   /* never blocks: the generator is seeded at boot */
        if (uc_put((uint8_t *)(uintptr_t)buf + at, chunk, k))
            return at ? (long)at : -EFAULT;
    }
    return (long)n;
}
