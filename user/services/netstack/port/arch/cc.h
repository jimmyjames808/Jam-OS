/* lwIP's compiler and platform layer for netstack (lwIP's "arch/cc.h",
 * found on lwIP's include path through user/services/netstack/port).
 *
 * Jam OS has no C library: the types come from the compiler's own
 * <stdint.h> and <stddef.h>, memcpy and friends from libos (port/string.h),
 * and the printf formats are written out here because there is no
 * <inttypes.h>. lwIP's diagnostics and failed assertions go to the
 * kernel log through port/sys_arch.c, rate-limited (a burst can't flood
 * the log); a failed assertion then ends netstack, whose supervisor starts
 * it again (an assertion means lwIP's own state is broken: going on could
 * only make it worse). */
#pragma once

#include <stdint.h>

#define LWIP_NO_INTTYPES_H 1    /* no <inttypes.h>: the formats are below */
#define LWIP_NO_CTYPE_H    1    /* no <ctype.h>: lwIP's own lwip_isdigit and the rest */
#define LWIP_NO_UNISTD_H   1    /* lwIP defines ssize_t itself */

#define X8_F  "02x"
#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "zu"

#define BYTE_ORDER LITTLE_ENDIAN

/* lwip_htons and lwip_htonl as single instructions instead of def.c's
 * shifts. */
#define lwip_htons(x) ((u16_t)__builtin_bswap16((u16_t)(x)))
#define lwip_htonl(x) ((u32_t)__builtin_bswap32((u32_t)(x)))

/* Packed protocol headers: GCC's attribute on the struct, nothing around it. */
#define PACK_STRUCT_STRUCT __attribute__((packed))

/* port/sys_arch.c */
void     lwport_diag(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void     lwport_assert(const char *msg, const char *file, int line) __attribute__((noreturn));
uint32_t lwport_random(void);

#define LWIP_PLATFORM_DIAG(x)   do { lwport_diag x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) lwport_assert((x), __FILE__, __LINE__)
#define LWIP_RAND()             lwport_random()
