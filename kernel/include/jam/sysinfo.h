/* System information for the shell (syscalls 130-133, kernel/abi/
 * sysc_sysinfo.c): the kernel halves, which the tests call directly. */
#pragma once

#include <stdint.h>
#include <jam/abi.h>
#include <jam/handle.h>
#include <jam/status.h>

/* OK if h in t is a RES_ROOT resource with RIGHT_READ (what every call
 * of the block needs); else the handle_get error, or ERR_WRONG_TYPE for
 * another kind of resource. */
status_t sysinfo_check_root(struct handle_table *t, handle_t h);
void     sysinfo_fill(struct sys_info *s);
void     sysinfo_cpu(uint32_t i, struct cpu_stat *s);
