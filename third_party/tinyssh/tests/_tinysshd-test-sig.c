/*
20260908
Jan Mojzis
Public domain.
*/

#include <signal.h>
#include "sig.h"

static void handler(int sig) { (void) sig; }

int main(void) {
    struct sigaction sa;

    sig_catch(SIGUSR1, handler);
    if (sigaction(SIGUSR1, (struct sigaction *) 0, &sa) == -1) return 111;
    if (sa.sa_handler != handler) return 111;
#ifdef SA_RESTART
    if (sa.sa_flags & SA_RESTART) return 111;
#endif
    if (raise(SIGUSR1) != 0) return 111;
    if (raise(SIGUSR1) != 0) return 111;
    return 0;
}
