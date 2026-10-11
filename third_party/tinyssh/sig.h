#ifndef SIG_H____
#define SIG_H____

#include <signal.h>

extern void sig_catch(int, void (*)(int));
#define sig_uncatch(s) (sig_catch((s), SIG_DFL))
#define sig_ignore(s) (sig_catch((s), SIG_IGN))

#endif
