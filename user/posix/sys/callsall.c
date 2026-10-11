/* __jam_calls_all: every handler of the layer, for a call whose number is
 * known only when it runs (musl's public syscall(), or a call site the
 * compiler couldn't reduce to its number): the override of syscall_arch.h
 * names this table there, which links every handler (calls.def says why
 * a program otherwise links only the handlers its call sites name). */
#include "calls.h"

#define LX(name)    lx_##name,
#define LX_CP(name) lx_##name,
lx_call *const __jam_calls_all[] = {
#include "calls.def"
};
