/* System call 150: random_get, bytes from the kernel's random number
 * generator (<jam/random.h>). No handle: anyone may ask, as anyone may read
 * the clock; the generator never blocks and a call is at most
 * RANDOM_GET_MAX bytes, so a caller spends its own time and nothing else.
 * The rules every sysc_* follows are in sysc.h. */
#include <stdint.h>
#include <jam/random.h>
#include <jam/string.h>
#include <jam/syscall_impl.h>
#include <jam/usercopy.h>
#include "sysc.h"

int64_t sysc_random_get(uint64_t buf, uint64_t len)
{
    if (len > RANDOM_GET_MAX)
        return ERR_INVALID_ARGS;
    if (len == 0)
        return OK;
    uint8_t bytes[RANDOM_GET_MAX];
    random_bytes(bytes, len);
    status_t st = copy_to_user(buf, bytes, len);
    explicit_bzero(bytes, len);
    return st == OK ? OK : ERR_INVALID_ARGS;
}
