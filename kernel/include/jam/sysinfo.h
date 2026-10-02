/* System information for the shell (syscalls 130-133, kernel/abi/
 * sysc_sysinfo.c): the kernel halves, which the tests call directly. */
#pragma once

#include <stdint.h>
#include <jam/abi.h>
#include <jam/handle.h>
#include <jam/status.h>

/* OK if h in t is a RES_ROOT resource with the rights `need` (each call
 * that takes the root checks for its own power, RIGHT_ROOT_*); else the
 * handle_get error (ERR_ACCESS_DENIED: a right missing), or
 * ERR_WRONG_TYPE for another kind of resource. */
status_t sysinfo_check_root(struct handle_table *t, handle_t h, rights_t need);
/* The version string sys_info reports (kernel/main.c: "0.0.27-m8.5"). */
extern const char jamos_version[];
void     sysinfo_fill(struct sys_info *s);
void     sysinfo_cpu(uint32_t i, struct cpu_stat *s);
