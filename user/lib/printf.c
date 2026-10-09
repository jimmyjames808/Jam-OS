/* The names of the status codes (status_str), for log lines. In libjam
 * (libos without its C library), so a POSIX program has it too; libos's
 * printf family is user/lib/stdio.c. */
#include <os.h>

const char *status_str(status_t s)
{
    switch (s) {
    case OK:                   return "OK";
    case ERR_INTERNAL:         return "ERR_INTERNAL";
    case ERR_NOT_SUPPORTED:    return "ERR_NOT_SUPPORTED";
    case ERR_NO_MEMORY:        return "ERR_NO_MEMORY";
    case ERR_INVALID_ARGS:     return "ERR_INVALID_ARGS";
    case ERR_BAD_HANDLE:       return "ERR_BAD_HANDLE";
    case ERR_WRONG_TYPE:       return "ERR_WRONG_TYPE";
    case ERR_ACCESS_DENIED:    return "ERR_ACCESS_DENIED";
    case ERR_BAD_STATE:        return "ERR_BAD_STATE";
    case ERR_OUT_OF_RANGE:     return "ERR_OUT_OF_RANGE";
    case ERR_BUFFER_TOO_SMALL: return "ERR_BUFFER_TOO_SMALL";
    case ERR_SHOULD_WAIT:      return "ERR_SHOULD_WAIT";
    case ERR_TIMED_OUT:        return "ERR_TIMED_OUT";
    case ERR_PEER_CLOSED:      return "ERR_PEER_CLOSED";
    case ERR_CANCELED:         return "ERR_CANCELED";
    case ERR_ALREADY_BOUND:    return "ERR_ALREADY_BOUND";
    case ERR_NOT_FOUND:        return "ERR_NOT_FOUND";
    case ERR_NO_RESOURCES:     return "ERR_NO_RESOURCES";
    case ERR_IO:               return "ERR_IO";
    case ERR_ALREADY_EXISTS:   return "ERR_ALREADY_EXISTS";
    case ERR_NO_SPACE:         return "ERR_NO_SPACE";
    case ERR_NOT_DIR:          return "ERR_NOT_DIR";
    case ERR_IS_DIR:           return "ERR_IS_DIR";
    case ERR_NOT_EMPTY:        return "ERR_NOT_EMPTY";
    case ERR_NAME_TOO_LONG:    return "ERR_NAME_TOO_LONG";
    case ERR_BUSY:             return "ERR_BUSY";
    }
    return "?";
}
