/* prctl: the options Chromium and its libraries use, answered, and the
 * rest refused the way Linux refuses an option it does not have (EINVAL,
 * named once in the ENOSYS log).
 *
 *   - PR_SET_NAME and PR_GET_NAME: a thread's name, 15 bytes and a NUL, kept
 *     here per thread (no kernel call: a thread's name is the program's
 *     own business). A thread that has not set one has the program's name,
 *     as a Linux thread starts with its program's.
 *   - PR_SET_DUMPABLE and PR_GET_DUMPABLE, PR_SET_NO_NEW_PRIVS and
 *     PR_GET_NO_NEW_PRIVS, PR_SET_PDEATHSIG and PR_GET_PDEATHSIG,
 *     PR_SET_TIMERSLACK and PR_GET_TIMERSLACK: the process's settings, kept
 *     and given back. None changes anything yet: there are no core dumps,
 *     no set-user-id programs, the timers have no slack and a child is not
 *     signalled when its parent ends. A program that sets them (Chromium
 *     does, between starting a child and its program) must not fail.
 *   - PR_SET_VMA with PR_SET_VMA_ANON_NAME: a name for a range of
 *     anonymous memory (PartitionAlloc names its pools); accepted and not
 *     kept.
 *   - PR_GET_SECCOMP is 0 (no filter); PR_SET_SECCOMP is EINVAL: Jam OS's
 *     sandbox is its own namespaces and grants, not a filter.
 *   - PR_SET_MDWE and PR_GET_MDWE (Linux's memory-deny-write-execute):
 *     Jam OS denies writable-and-executable memory, and execute added to
 *     memory, to every process but one holding the JIT power (SR_JIT,
 *     docs/CR-PLAN.md section 12), and then only in JIT memory. So
 *     PR_MDWE_REFUSE_EXEC_GAIN is in force exactly when the process holds
 *     no power (PR_GET_MDWE), and PR_SET_MDWE with it gives the power up
 *     for good: SR_JIT closed, no new JIT memory (MAP_JIT is EPERM), and
 *     none for a child forked or a program exec'd after (Linux's rule:
 *     inherited, never undone). The JIT memory already mapped stays as it
 *     is (Linux would refuse execute added to it; a program sets this at
 *     its start, before it has any: Chromium's processes that aren't
 *     renderers). PR_MDWE_NO_INHERIT (a child free of it) can't be given
 *     back: EINVAL, as a kernel without it answers. A process with no
 *     power that clears it gets EPERM, as Linux's does once it is set.
 *
 * The settings are the process's, shared by its threads, so they are
 * atomics; a name is the thread's own. */
#include <errno.h>
#include <string.h>
#include "layer.h"

/* Linux's option numbers (include/uapi/linux/prctl.h). */
#define PR_SET_PDEATHSIG     1
#define PR_GET_PDEATHSIG     2
#define PR_GET_DUMPABLE      3
#define PR_SET_DUMPABLE      4
#define PR_SET_NAME          15
#define PR_GET_NAME          16
#define PR_GET_SECCOMP       21
#define PR_SET_SECCOMP       22
#define PR_SET_TIMERSLACK    29
#define PR_GET_TIMERSLACK    30
#define PR_SET_NO_NEW_PRIVS  38
#define PR_GET_NO_NEW_PRIVS  39
#define PR_SET_MDWE          65
#define PR_GET_MDWE          66
#define PR_MDWE_REFUSE_EXEC_GAIN 1ul
#define PR_SET_VMA           0x53564d41
#define PR_SET_VMA_ANON_NAME 0

#define NAME_BYTES      16        /* a thread's name with its NUL (Linux's TASK_COMM_LEN) */
#define SIGNAL_MAX      64        /* the highest signal PR_SET_PDEATHSIG takes */
#define SLACK_DEFAULT   50000ul   /* ns: Linux's default timer slack */

static __thread char thread_name[NAME_BYTES];   /* "" until the thread sets one */

static int dumpable = 1;                  /* PR_SET_DUMPABLE: 0 or 1 */
static int no_new_privs;                  /* PR_SET_NO_NEW_PRIVS: 0 or 1, once 1 stays */
static int pdeathsig;                     /* PR_SET_PDEATHSIG: a signal or 0 */
static unsigned long timer_slack = SLACK_DEFAULT;

static long set_name(long addr)
{
    const char *src = (const char *)(uintptr_t)addr;
    char name[NAME_BYTES];
    long n = uc_strnlen(src, NAME_BYTES - 1);   /* the caller's */
    if (n < 0 || uc_get(name, src, (size_t)n))
        return -EFAULT;
    memset(name + n, 0, NAME_BYTES - (size_t)n);
    memcpy(thread_name, name, NAME_BYTES);
    return 0;
}

static long get_name(long addr)
{
    char dst[NAME_BYTES] = { 0 };
    const char *name = thread_name[0] ? thread_name : program_invocation_short_name;
    if (name)
        strncpy(dst, name, NAME_BYTES - 1);
    return uc_put((void *)(uintptr_t)addr, dst, NAME_BYTES);
}

static long put_int(long addr, int v)
{
    return uc_put_int(addr, v);
}

long lx_prctl(long option_l, long a2, long a3, long a4, long a5, long f)
{
    int option = (int)option_l;   /* Linux's int; the rest of the register is not defined */
    switch (option) {
    case PR_SET_NAME:
        return set_name(a2);
    case PR_GET_NAME:
        return get_name(a2);
    case PR_SET_DUMPABLE:
        if (a2 != 0 && a2 != 1)
            return -EINVAL;
        __atomic_store_n(&dumpable, (int)a2, __ATOMIC_RELAXED);
        return 0;
    case PR_GET_DUMPABLE:
        return __atomic_load_n(&dumpable, __ATOMIC_RELAXED);
    case PR_SET_NO_NEW_PRIVS:   /* only ever set, never cleared: Linux's rule */
        if (a2 != 1 || a3 || a4 || a5)
            return -EINVAL;
        __atomic_store_n(&no_new_privs, 1, __ATOMIC_RELAXED);
        return 0;
    case PR_GET_NO_NEW_PRIVS:
        if (a2 || a3 || a4 || a5)
            return -EINVAL;
        return __atomic_load_n(&no_new_privs, __ATOMIC_RELAXED);
    case PR_SET_PDEATHSIG:
        if (a2 < 0 || a2 > SIGNAL_MAX)
            return -EINVAL;
        __atomic_store_n(&pdeathsig, (int)a2, __ATOMIC_RELAXED);
        return 0;
    case PR_GET_PDEATHSIG:
        return put_int(a2, __atomic_load_n(&pdeathsig, __ATOMIC_RELAXED));
    case PR_SET_TIMERSLACK:   /* 0 puts the default back */
        __atomic_store_n(&timer_slack, a2 ? (unsigned long)a2 : SLACK_DEFAULT, __ATOMIC_RELAXED);
        return 0;
    case PR_GET_TIMERSLACK:
        return (long)__atomic_load_n(&timer_slack, __ATOMIC_RELAXED);
    case PR_SET_VMA:
        if (a2 != PR_SET_VMA_ANON_NAME)
            break;
        return 0;
    case PR_GET_SECCOMP:
        return 0;
    case PR_SET_MDWE:
        if (((unsigned long)a2 & ~PR_MDWE_REFUSE_EXEC_GAIN) || a3 || a4 || a5)
            return -EINVAL;
        if (a2)
            mem_jit_give_up();
        else if (!mem_jit_power())
            return -EPERM;   /* in force: never undone */
        return 0;
    case PR_GET_MDWE:
        if (a2 || a3 || a4 || a5)
            return -EINVAL;
        return mem_jit_power() ? 0 : (long)PR_MDWE_REFUSE_EXEC_GAIN;
    default:
        break;
    }
    lx_enosys_option("prctl option", option);
    return -EINVAL;
}
