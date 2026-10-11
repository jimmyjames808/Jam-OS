/* exit: end this shell, and with it its terminal (and every program it
 * started with &): init closes the terminal when its shell ends with
 * code 0, the first terminal like any other. A shell init gave no
 * "term=" (sh_term_no 0: started without a compositor) would only be
 * started again, so exit says so and does nothing there. A session's
 * shell ("session") ends with the code given, which init passes on as the
 * session's (session_ctl.ended): `ssh pc 'exit 3'` ends with 3. */
#include "sh.h"

SH_CMD(exit)
{
    uint64_t code = 0;
    if (argc > 2 || (argc == 2 && (!sh_parse_u64(argv[1], &code) || code > 255))) {
        sh_tty("usage: exit [code]   (code 0 to 255)\n");
        return 2;
    }
    if (!sh_term_no && !sh_session) {
        sh_tty("exit: this is the only terminal (no desktop), so it stays open\n");
        return 1;
    }
    sh_flush();
    /* A terminal's shell ends with 0 whatever the code: init closes the
     * terminal then (any other code is a crash to it, started again). */
    jam_process_exit(sh_session ? (int64_t)code : 0);
    return 0;   /* not reached */
}
