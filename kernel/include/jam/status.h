/* Status codes returned by object and handle operations (and, from M5,
 * by system calls). 0 is success; errors are negative. */
#pragma once

typedef int status_t;

#define OK                  0
#define ERR_INTERNAL       -1
#define ERR_NOT_SUPPORTED  -2
#define ERR_NO_MEMORY      -3
#define ERR_INVALID_ARGS   -4
#define ERR_BAD_HANDLE     -5
#define ERR_WRONG_TYPE     -6
#define ERR_ACCESS_DENIED  -7
#define ERR_BAD_STATE      -8
#define ERR_OUT_OF_RANGE   -9
#define ERR_BUFFER_TOO_SMALL -10
#define ERR_SHOULD_WAIT    -11   /* would block: nothing to read / no room */
#define ERR_TIMED_OUT      -12
#define ERR_PEER_CLOSED    -13
#define ERR_CANCELED       -14
#define ERR_ALREADY_BOUND  -15
#define ERR_NOT_FOUND      -16
#define ERR_NO_RESOURCES   -17   /* a table or queue is full */

const char *status_str(status_t s);
