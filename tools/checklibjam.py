#!/usr/bin/env python3
"""Check that libjam.a (libos without its C library, <os.h>) defines no
name a C library defines, so a POSIX program can link it with musl.

    checklibjam.py <nm> <libjam.a> [<libc.a>]

Every global symbol libjam.a defines is looked up in the C library's
names: those <libc.a> defines when it is given (musl's, once it is
built), else the list below (the C standard's and POSIX's functions and
objects, and the start-up names a C library owns). A name in both would
be defined twice in a POSIX program: whichever object the linker took
first would win, or the link would fail. Prints the clashes and exits 1
if there are any.

The Makefile's other half of the test links a stand-in C library
(tools/libjam-stub.c) with every object of libjam.a, --fatal-warnings:
that fails if libjam needs a C library function the stand-in lacks, or
defines one the stand-in has.

Run from the repository root (`make check` does)."""
import subprocess
import sys

LIBC_NAMES = set("""
_start _init _fini main __libc_start_main environ __environ errno __errno_location
memcpy memmove memset memcmp memchr memrchr memmem mempcpy memccpy bzero bcmp bcopy
explicit_bzero strcpy strncpy strcat strncat strcmp strncmp strcoll strxfrm strchr
strrchr strchrnul strcspn strspn strpbrk strstr strcasestr strtok strtok_r strlen strnlen
strerror strerror_r strdup strndup strsignal stpcpy stpncpy strcasecmp strncasecmp
strlcpy strlcat strsep index rindex ffs ffsl ffsll
malloc calloc realloc free aligned_alloc posix_memalign memalign valloc pvalloc
reallocarray malloc_usable_size abort atexit at_quick_exit exit _Exit _exit quick_exit
getenv secure_getenv setenv unsetenv putenv clearenv system atoi atol atoll atof strtol
strtoul strtoll strtoull strtod strtof strtold strtoimax strtoumax abs labs llabs div
ldiv lldiv rand srand rand_r random srandom initstate setstate qsort qsort_r bsearch
mblen mbtowc wctomb mbstowcs wcstombs mkstemp mkostemp mkdtemp mktemp realpath getsubopt
grantpt ptsname ptsname_r unlockpt posix_openpt drand48 erand48 lrand48 nrand48 mrand48
jrand48 srand48 seed48 lcong48 arc4random getrandom getentropy
printf fprintf sprintf snprintf vprintf vfprintf vsprintf vsnprintf dprintf vdprintf
asprintf vasprintf scanf fscanf sscanf vscanf vsscanf vfscanf puts fputs putchar fputc
putc getchar fgetc getc fgets gets ungetc fopen freopen fclose fflush fread fwrite fseek
fseeko ftell ftello rewind fgetpos fsetpos feof ferror clearerr perror remove rename
renameat tmpfile tmpnam tempnam setbuf setvbuf setlinebuf fdopen fileno popen pclose
getline getdelim fmemopen open_memstream stdin stdout stderr flockfile funlockfile
isalnum isalpha isblank iscntrl isdigit isgraph islower isprint ispunct isspace isupper
isxdigit tolower toupper isascii toascii
read write close lseek pread pwrite readv writev dup dup2 dup3 pipe pipe2 chdir fchdir
getcwd unlink unlinkat rmdir link linkat symlink symlinkat readlink readlinkat access
faccessat isatty ttyname ttyname_r getpid getppid gettid getuid geteuid getgid getegid
setuid setgid seteuid setegid getgroups setgroups getpgid setpgid getpgrp setsid getsid
fork vfork execve execv execvp execvpe execl execlp execle fexecve sleep usleep alarm
pause sysconf pathconf fpathconf gethostname sethostname getpagesize truncate ftruncate
fsync fdatasync sync getopt getopt_long optarg optind opterr optopt brk sbrk nice
chown fchown lchown fchownat chmod fchmod fchmodat umask getlogin getlogin_r
open openat creat fcntl ioctl stat fstat lstat fstatat mkdir mkdirat mknod mkfifo
statfs fstatfs statvfs fstatvfs utime utimes utimensat futimens
mmap munmap mprotect madvise msync mlock munlock mremap shm_open shm_unlink memfd_create
poll ppoll select pselect epoll_create epoll_create1 epoll_ctl epoll_wait epoll_pwait
eventfd socket socketpair bind connect listen accept accept4 send recv sendto recvfrom
sendmsg recvmsg shutdown getsockopt setsockopt getsockname getpeername getaddrinfo
freeaddrinfo gai_strerror getnameinfo gethostbyname gethostbyaddr inet_ntoa inet_addr
inet_aton inet_pton inet_ntop htons htonl ntohs ntohl
opendir fdopendir readdir readdir_r closedir rewinddir seekdir telldir dirfd scandir
alphasort
time clock clock_gettime clock_settime clock_getres clock_nanosleep nanosleep
gettimeofday settimeofday localtime gmtime mktime strftime strptime asctime ctime
difftime timegm localtime_r gmtime_r asctime_r ctime_r tzset tzname timezone daylight
times getrusage getitimer setitimer timer_create timer_settime timer_gettime timer_delete
signal raise kill killpg sigaction sigprocmask sigemptyset sigfillset sigaddset sigdelset
sigismember sigsuspend sigpending sigwait sigwaitinfo sigtimedwait sigaltstack psignal
setjmp longjmp _setjmp _longjmp sigsetjmp siglongjmp __assert_fail
pthread_create pthread_join pthread_detach pthread_exit pthread_self pthread_equal
pthread_kill pthread_cancel pthread_once pthread_mutex_init pthread_mutex_lock
pthread_mutex_trylock pthread_mutex_unlock pthread_mutex_destroy pthread_cond_init
pthread_cond_wait pthread_cond_timedwait pthread_cond_signal pthread_cond_broadcast
pthread_cond_destroy pthread_key_create pthread_getspecific pthread_setspecific
pthread_sigmask sched_yield wait waitpid waitid wait3 wait4 posix_spawn posix_spawnp
uname syscall getpwnam getpwuid getgrnam getgrgid setlocale localeconv
sqrt sin cos tan exp log log2 log10 pow floor ceil fabs fmod round trunc
wcslen wcscpy wcscmp wcsncmp wcschr wcsrchr mbrtowc wcrtomb mbsrtowcs wcsrtombs
""".split())


def defined(nm, archive):
    out = subprocess.run([nm, "-g", "--defined-only", archive], capture_output=True,
                         text=True, check=True).stdout
    names = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            names.add(parts[2])
    return names


def main(args):
    if len(args) not in (2, 3):
        print(__doc__)
        return 2
    nm, libjam = args[0], args[1]
    libc = defined(nm, args[2]) if len(args) == 3 else LIBC_NAMES
    clashes = sorted(defined(nm, libjam) & libc)
    for name in clashes:
        print("checklibjam: libjam.a defines %s, a C library name" % name)
    print("checklibjam: %s" % ("FAIL" if clashes else "PASS"))
    return 1 if clashes else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
