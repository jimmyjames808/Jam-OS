/* Linux's system call names, for the ENOSYS log (enosys.c): a call number
 * no handler answers is easier to read as "syscall 435 (clone3)".
 *
 * This is one of the layer's files that sees Linux's numbers
 * (<sys/syscall.h>, whose SYS_* names clash with Jam OS's own), so it
 * includes nothing of Jam OS's. The list is musl's x86-64 table, in
 * number order; each number comes from its SYS_ constant, so a name that
 * musl does not have fails the build instead of naming the wrong call.
 * The names are one string, found by walking it beside the numbers: the
 * log is rare, and a table of pointers would cost every program 3 KiB
 * more. */
#include <string.h>
#include <sys/syscall.h>
#include "calls.h"

#define NAMES(N) \
    N(read) N(write) N(open) N(close) N(stat) N(fstat) N(lstat) N(poll) N(lseek) N(mmap) \
    N(mprotect) N(munmap) N(brk) N(rt_sigaction) N(rt_sigprocmask) N(rt_sigreturn) \
    N(ioctl) N(pread64) N(pwrite64) N(readv) N(writev) N(access) N(pipe) N(select) \
    N(sched_yield) N(mremap) N(msync) N(mincore) N(madvise) N(shmget) N(shmat) N(shmctl) \
    N(dup) N(dup2) N(pause) N(nanosleep) N(getitimer) N(alarm) N(setitimer) N(getpid) \
    N(sendfile) N(socket) N(connect) N(accept) N(sendto) N(recvfrom) N(sendmsg) \
    N(recvmsg) N(shutdown) N(bind) N(listen) N(getsockname) N(getpeername) N(socketpair) \
    N(setsockopt) N(getsockopt) N(clone) N(fork) N(vfork) N(execve) N(exit) N(wait4) \
    N(kill) N(uname) N(semget) N(semop) N(semctl) N(shmdt) N(msgget) N(msgsnd) N(msgrcv) \
    N(msgctl) N(fcntl) N(flock) N(fsync) N(fdatasync) N(truncate) N(ftruncate) \
    N(getdents) N(getcwd) N(chdir) N(fchdir) N(rename) N(mkdir) N(rmdir) N(creat) \
    N(link) N(unlink) N(symlink) N(readlink) N(chmod) N(fchmod) N(chown) N(fchown) \
    N(lchown) N(umask) N(gettimeofday) N(getrlimit) N(getrusage) N(sysinfo) N(times) \
    N(ptrace) N(getuid) N(syslog) N(getgid) N(setuid) N(setgid) N(geteuid) N(getegid) \
    N(setpgid) N(getppid) N(getpgrp) N(setsid) N(setreuid) N(setregid) N(getgroups) \
    N(setgroups) N(setresuid) N(getresuid) N(setresgid) N(getresgid) N(getpgid) \
    N(setfsuid) N(setfsgid) N(getsid) N(capget) N(capset) N(rt_sigpending) \
    N(rt_sigtimedwait) N(rt_sigqueueinfo) N(rt_sigsuspend) N(sigaltstack) N(utime) \
    N(mknod) N(uselib) N(personality) N(ustat) N(statfs) N(fstatfs) N(sysfs) \
    N(getpriority) N(setpriority) N(sched_setparam) N(sched_getparam) \
    N(sched_setscheduler) N(sched_getscheduler) N(sched_get_priority_max) \
    N(sched_get_priority_min) N(sched_rr_get_interval) N(mlock) N(munlock) N(mlockall) \
    N(munlockall) N(vhangup) N(modify_ldt) N(pivot_root) N(_sysctl) N(prctl) \
    N(arch_prctl) N(adjtimex) N(setrlimit) N(chroot) N(sync) N(acct) N(settimeofday) \
    N(mount) N(umount2) N(swapon) N(swapoff) N(reboot) N(sethostname) N(setdomainname) \
    N(iopl) N(ioperm) N(create_module) N(init_module) N(delete_module) \
    N(get_kernel_syms) N(query_module) N(quotactl) N(nfsservctl) N(getpmsg) N(putpmsg) \
    N(afs_syscall) N(tuxcall) N(security) N(gettid) N(readahead) N(setxattr) \
    N(lsetxattr) N(fsetxattr) N(getxattr) N(lgetxattr) N(fgetxattr) N(listxattr) \
    N(llistxattr) N(flistxattr) N(removexattr) N(lremovexattr) N(fremovexattr) N(tkill) \
    N(time) N(futex) N(sched_setaffinity) N(sched_getaffinity) N(set_thread_area) \
    N(io_setup) N(io_destroy) N(io_getevents) N(io_submit) N(io_cancel) \
    N(get_thread_area) N(lookup_dcookie) N(epoll_create) N(epoll_ctl_old) \
    N(epoll_wait_old) N(remap_file_pages) N(getdents64) N(set_tid_address) \
    N(restart_syscall) N(semtimedop) N(fadvise64) N(timer_create) N(timer_settime) \
    N(timer_gettime) N(timer_getoverrun) N(timer_delete) N(clock_settime) \
    N(clock_gettime) N(clock_getres) N(clock_nanosleep) N(exit_group) N(epoll_wait) \
    N(epoll_ctl) N(tgkill) N(utimes) N(vserver) N(mbind) N(set_mempolicy) \
    N(get_mempolicy) N(mq_open) N(mq_unlink) N(mq_timedsend) N(mq_timedreceive) \
    N(mq_notify) N(mq_getsetattr) N(kexec_load) N(waitid) N(add_key) N(request_key) \
    N(keyctl) N(ioprio_set) N(ioprio_get) N(inotify_init) N(inotify_add_watch) \
    N(inotify_rm_watch) N(migrate_pages) N(openat) N(mkdirat) N(mknodat) N(fchownat) \
    N(futimesat) N(newfstatat) N(unlinkat) N(renameat) N(linkat) N(symlinkat) \
    N(readlinkat) N(fchmodat) N(faccessat) N(pselect6) N(ppoll) N(unshare) \
    N(set_robust_list) N(get_robust_list) N(splice) N(tee) N(sync_file_range) \
    N(vmsplice) N(move_pages) N(utimensat) N(epoll_pwait) N(signalfd) N(timerfd_create) \
    N(eventfd) N(fallocate) N(timerfd_settime) N(timerfd_gettime) N(accept4) \
    N(signalfd4) N(eventfd2) N(epoll_create1) N(dup3) N(pipe2) N(inotify_init1) \
    N(preadv) N(pwritev) N(rt_tgsigqueueinfo) N(perf_event_open) N(recvmmsg) \
    N(fanotify_init) N(fanotify_mark) N(prlimit64) N(name_to_handle_at) \
    N(open_by_handle_at) N(clock_adjtime) N(syncfs) N(sendmmsg) N(setns) N(getcpu) \
    N(process_vm_readv) N(process_vm_writev) N(kcmp) N(finit_module) N(sched_setattr) \
    N(sched_getattr) N(renameat2) N(seccomp) N(getrandom) N(memfd_create) \
    N(kexec_file_load) N(bpf) N(execveat) N(userfaultfd) N(membarrier) N(mlock2) \
    N(copy_file_range) N(preadv2) N(pwritev2) N(pkey_mprotect) N(pkey_alloc) \
    N(pkey_free) N(statx) N(io_pgetevents) N(rseq) N(uretprobe) N(uprobe) \
    N(pidfd_send_signal) N(io_uring_setup) N(io_uring_enter) N(io_uring_register) \
    N(open_tree) N(move_mount) N(fsopen) N(fsconfig) N(fsmount) N(fspick) N(pidfd_open) \
    N(clone3) N(close_range) N(openat2) N(pidfd_getfd) N(faccessat2) N(process_madvise) \
    N(epoll_pwait2) N(mount_setattr) N(quotactl_fd) N(landlock_create_ruleset) \
    N(landlock_add_rule) N(landlock_restrict_self) N(memfd_secret) N(process_mrelease) \
    N(futex_waitv) N(set_mempolicy_home_node) N(cachestat) N(fchmodat2) \
    N(map_shadow_stack) N(futex_wake) N(futex_wait) N(futex_requeue) N(statmount) \
    N(listmount) N(lsm_get_self_attr) N(lsm_set_self_attr) N(lsm_list_modules) N(mseal) \
    N(setxattrat) N(getxattrat) N(listxattrat) N(removexattrat) N(open_tree_attr) \
    N(file_getattr) N(file_setattr) N(listns)

#define NUMBER(name) (unsigned short)SYS_##name,
#define STRING(name) #name "\0"

static const unsigned short numbers[] = { NAMES(NUMBER) };
static const char names[] = NAMES(STRING);

const char *lx_sysname(long n)
{
    const char *s = names;
    for (unsigned i = 0; i < sizeof(numbers) / sizeof(numbers[0]); i++) {
        if (numbers[i] == n)
            return s;
        s += strlen(s) + 1;
    }
    return NULL;
}
