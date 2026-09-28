/* M5 foundation: a weak stand-in for every interface function in the M5
 * headers, so each track (M5-PLAN.md) builds and boots on its own. A track
 * replaces the ones it owns by defining the real function (the strong
 * symbol wins at link time). Phase 2 deletes this file. */
#include <jam/aspace.h>
#include <jam/bootfs.h>
#include <jam/elf.h>
#include <jam/kprintf.h>
#include <jam/panic.h>
#include <jam/sched.h>
#include <jam/syscall.h>
#include <jam/trap.h>
#include <jam/uentry.h>
#include <jam/usercopy.h>

#define WEAK __attribute__((weak))

/* Track A: entry path */
WEAK _Noreturn void arch_enter_user(uint64_t entry, uint64_t stack, uint64_t arg0, uint64_t arg1)
{
    (void)stack, (void)arg0, (void)arg1;
    panic("arch_enter_user(%lx): no ring-3 support in this build", entry);
}
WEAK void arch_thread_switch(struct thread *prev, struct thread *next)
{
    (void)prev, (void)next;
}
WEAK bool trap_page_fault(struct trap_frame *f)
{
    (void)f;
    return false;
}
WEAK _Noreturn void user_fault_kill(struct trap_frame *f, const char *why)
{
    (void)f;
    panic("user fault (%s) with no user support in this build", why);
}
WEAK int fpu_ustate_alloc(struct thread *t)
{
    (void)t;
    return ERR_NOT_SUPPORTED;
}
WEAK void fpu_ustate_free(struct thread *t)
{
    (void)t;
}
WEAK status_t copy_from_user(void *dst, uint64_t usrc, size_t n)
{
    (void)dst, (void)usrc, (void)n;
    return ERR_NOT_SUPPORTED;
}
WEAK status_t copy_to_user(uint64_t udst, const void *src, size_t n)
{
    (void)udst, (void)src, (void)n;
    return ERR_NOT_SUPPORTED;
}
WEAK status_t copy_str_from_user(char *dst, uint64_t usrc, size_t cap, size_t *len)
{
    (void)dst, (void)usrc, (void)cap, (void)len;
    return ERR_NOT_SUPPORTED;
}

/* Track B: address spaces */
WEAK status_t aspace_create(struct aspace **out)
{
    (void)out;
    return ERR_NOT_SUPPORTED;
}
WEAK void aspace_ref(struct aspace *as)
{
    (void)as;
}
WEAK void aspace_unref(struct aspace *as)
{
    (void)as;
}
WEAK status_t aspace_map(struct aspace *as, struct vmo *vmo, uint64_t vmo_off, uint64_t len,
                         unsigned flags, uint64_t *addr)
{
    (void)as, (void)vmo, (void)vmo_off, (void)len, (void)flags, (void)addr;
    return ERR_NOT_SUPPORTED;
}
WEAK status_t aspace_unmap(struct aspace *as, uint64_t addr, uint64_t len)
{
    (void)as, (void)addr, (void)len;
    return ERR_NOT_SUPPORTED;
}
WEAK status_t aspace_protect(struct aspace *as, uint64_t addr, uint64_t len, unsigned flags)
{
    (void)as, (void)addr, (void)len, (void)flags;
    return ERR_NOT_SUPPORTED;
}
WEAK status_t aspace_fault(struct aspace *as, uint64_t addr, unsigned access)
{
    (void)as, (void)addr, (void)access;
    return ERR_NOT_FOUND;
}
WEAK void aspace_switch(struct aspace *prev, struct aspace *next)
{
    (void)prev, (void)next;
}
WEAK uint64_t aspace_pml4(struct aspace *as)
{
    (void)as;
    return 0;
}

/* Track C: syscall table, bootfs, ELF */
WEAK int64_t syscall_dispatch(struct syscall_frame *f)
{
    (void)f;
    return ERR_NOT_SUPPORTED;
}
WEAK void bootfs_init(const struct boot_info *bi)
{
    (void)bi;
}
WEAK status_t bootfs_find(const char *name, struct vmo **out, uint64_t *size)
{
    (void)name, (void)out, (void)size;
    return ERR_NOT_FOUND;
}
WEAK status_t bootfs_data(const char *name, const void **data, uint64_t *size)
{
    (void)name, (void)data, (void)size;
    return ERR_NOT_FOUND;
}
WEAK unsigned bootfs_count(void)
{
    return 0;
}
WEAK const char *bootfs_name(unsigned i)
{
    (void)i;
    return NULL;
}
WEAK status_t elf_parse(const void *image, uint64_t size, struct elf_plan *out)
{
    (void)image, (void)size, (void)out;
    return ERR_NOT_SUPPORTED;
}
