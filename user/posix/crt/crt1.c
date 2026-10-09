/* crt1.o: a POSIX program's entry. Overrides musl's crt/crt1.c
 * (user/posix/musl/OVERRIDES), whose _start takes Linux's stack block
 * (argc, argv, envp, auxv) from rsp. A Jam OS process starts with the
 * startup channel in rdi instead, rsp the top of a fresh stack and every
 * other register zero; __jam_start (start.c, in libc.a) reads the startup
 * message and builds Linux's block for musl's __libc_start_main.
 *
 * Only what must be in the program's own crt1.o is here: the entry, and
 * the references to main, _init and _fini (crti.o, crtn.o). */
#include "glue.h"

int main();
__attribute__((weak)) void _init(void);
__attribute__((weak)) void _fini(void);

__attribute__((used)) _Noreturn void _start_c(unsigned ch);

__asm__(
    ".text\n"
    ".global _start\n"
    ".type _start, @function\n"
    "_start:\n"
    "    xor %ebp, %ebp\n"     /* the end of the frame-pointer chain */
    "    and $-16, %rsp\n"     /* 16-aligned at the call */
    "    call _start_c\n"      /* rdi, the startup channel, passes straight through */
    "    ud2\n"
    ".size _start, . - _start\n");

_Noreturn void _start_c(unsigned ch)
{
    __jam_start(ch, main, _init, _fini);
}
