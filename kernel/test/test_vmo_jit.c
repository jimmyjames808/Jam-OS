/* Tests for JIT memory (vmo_make_jit, docs/CR-PLAN.md section 12): the
 * one exception to W^X. A JIT VMO may be mapped writable and executable
 * at once, through a handle with both rights; every other VMO keeps W^X
 * whatever the caller asks (the kernel's own aspace_map included), and a
 * JIT VMO is never sealed, never given a child, never made from a VMO
 * that holds anything. The argument is at the top of mm/aspace.c.
 *
 * The names start "vmo_jit" (the kind, the call) and "aspace_jit" (the
 * mapping rules), so `ktest=vmo` and `ktest=aspace` each run their half.
 * Nothing here enters ring 3: entries are checked by walking the tables
 * (aspace_pte), and utest's jit test runs code it wrote. */
#include <jam/aspace.h>
#include <jam/aspace_vmo.h>
#include <jam/cpu.h>
#include <jam/dbghook.h>
#include <jam/handle.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/pager.h>
#include <jam/resource.h>
#include <jam/resource_impl.h>
#include <jam/sys.h>
#include <jam/vmar.h>
#include <jam/vmo.h>

#define PG  PAGE_SIZE
#define R   ASPACE_READ
#define RW  (ASPACE_READ | ASPACE_WRITE)
#define RX  (ASPACE_READ | ASPACE_EXEC)
#define RWX (ASPACE_READ | ASPACE_WRITE | ASPACE_EXEC)
#define CAN_ALL (ASPACE_CAN_READ | ASPACE_CAN_WRITE | ASPACE_CAN_EXEC)

#define PTE_P  (1ull << 0)
#define PTE_W  (1ull << 1)
#define PTE_NX (1ull << 63)

static void put(struct vmo *v)
{
    kobject_unref(vmo_kobject(v));
}

static struct vmo *new_jit(uint64_t size)
{
    struct vmo *v;
    KT_EQ(vmo_create(size, 0, &v), OK);
    KT_EQ(vmo_make_jit(v), OK);
    KT_ASSERT(vmo_is_jit(v));
    return v;
}

/* The entry for va is present, writable and executable (NX clear). */
static bool pte_wx(struct aspace *as, uint64_t va)
{
    uint64_t e = aspace_pte(as, va);
    return (e & PTE_P) && (e & PTE_W) && !(e & PTE_NX);
}

/* Executable as far as the CPU can tell: NX clear, or no NX at all. */
static bool pte_x(struct aspace *as, uint64_t va)
{
    uint64_t e = aspace_pte(as, va);
    return (e & PTE_P) && (!(e & PTE_NX) || !cpu_features.nx);
}

/* ---- the kind --------------------------------------------------------------- */

/* What vmo_make_jit refuses, and that a refusal leaves the VMO as it was. */
KTEST(vmo_jit_make_refusals)
{
    struct vmo *v;
    /* Not a plain paged VMO: contiguous, kept, a pager VMO, a pager VMO's child. */
    KT_EQ(vmo_create(PG, VMO_CONTIGUOUS, &v), OK);
    KT_EQ(vmo_make_jit(v), ERR_NOT_SUPPORTED);
    put(v);
    KT_EQ(vmo_create(PG, VMO_KEEP_PAGES, &v), OK);
    KT_EQ(vmo_make_jit(v), ERR_NOT_SUPPORTED);
    KT_ASSERT(!vmo_is_jit(v));
    put(v);
    struct pager *pg;
    KT_EQ(pager_create(NULL, 16, PAGER_ANSWER_MAX_NS, &pg), OK);
    struct vmo *pv, *pc;
    KT_EQ(pager_create_vmo(pg, 1, 2 * PG, &pv), OK);
    KT_EQ(vmo_make_jit(pv), ERR_NOT_SUPPORTED);
    KT_EQ(vmo_create_child(NULL, pv, VMO_CHILD_SNAPSHOT_AT_LEAST_ON_WRITE, 0, 2 * PG, &pc), OK);
    KT_EQ(vmo_make_jit(pc), ERR_NOT_SUPPORTED);
    put(pc);
    put(pv);
    struct handle_table t;
    handle_table_init(&t);
    struct khandle kh = khandle_from_new(pager_kobject(pg), PAGER_RIGHTS);
    handle_t h;
    KT_EQ(handle_insert(&t, &kh, &h), OK);
    handle_table_destroy(&t);   /* the pager's last handle and reference */

    /* A page in it (written, or committed): it must start empty. */
    KT_EQ(vmo_create(2 * PG, 0, &v), OK);
    KT_EQ(vmo_write(v, PG, "x", 1), OK);
    KT_EQ(vmo_make_jit(v), ERR_BAD_STATE);
    KT_ASSERT(!vmo_is_jit(v));
    KT_EQ(vmo_decommit(v, 0, 2 * PG), OK);   /* empty again: now it may */
    KT_EQ(vmo_make_jit(v), OK);
    KT_EQ(vmo_make_jit(v), ERR_BAD_STATE);   /* once */
    put(v);

    /* A mapping (user or kernel), even with no page touched. */
    KT_EQ(vmo_create(PG, 0, &v), OK);
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    uint64_t a = 0;
    KT_EQ(aspace_map(as, v, 0, PG, R | CAN_ALL, &a), OK);
    KT_EQ(vmo_make_jit(v), ERR_BAD_STATE);
    KT_EQ(aspace_unmap(as, a, PG), OK);
    KT_EQ(vmo_committed(v), 0);
    KT_EQ(vmo_make_jit(v), OK);
    aspace_unref(as);
    put(v);

    /* Sealed (approved code): never JIT memory. */
    KT_EQ(vmo_create(PG, 0, &v), OK);
    KT_EQ(vmo_seal(v), OK);
    KT_EQ(vmo_make_jit(v), ERR_BAD_STATE);
    KT_ASSERT(!vmo_is_jit(v));
    put(v);
}

/* A JIT VMO is never sealed (so never approved code, and approved code is
 * never writable) and gets no copy-on-write child; it still reads, writes,
 * commits, decommits and resizes as any VMO. */
KTEST(vmo_jit_kind_refusals)
{
    struct vmo *v = new_jit(4 * PG), *c;
    KT_EQ(vmo_seal(v), ERR_BAD_STATE);
    KT_EQ(vmo_create_child(NULL, v, VMO_CHILD_SNAPSHOT, 0, PG, &c), ERR_NOT_SUPPORTED);
    KT_EQ(vmo_create_child(NULL, v, VMO_CHILD_SNAPSHOT_AT_LEAST_ON_WRITE, 0, PG, &c),
          ERR_NOT_SUPPORTED);
    KT_EQ(vmo_write(v, 0, "\xc3", 1), OK);
    uint8_t b = 0;
    KT_EQ(vmo_read(v, 0, &b, 1), OK);
    KT_EQ(b, 0xc3);
    KT_EQ(vmo_commit(v, PG, PG), OK);
    KT_EQ(vmo_committed(v), 2 * PG);
    KT_EQ(vmo_decommit(v, 0, 2 * PG), OK);
    KT_EQ(vmo_committed(v), 0);
    KT_EQ(vmo_set_size(v, 8 * PG), OK);
    KT_EQ(vmo_set_size(v, PG), OK);
    KT_ASSERT(vmo_is_jit(v));   /* for good */
    put(v);
}

/* ---- the mappings ------------------------------------------------------------ */

/* W+X: refused on a VMO that isn't JIT even with every CAN bit (the kernel's
 * own caller included), allowed on a JIT VMO; the entries a fault makes are
 * writable and executable, and a decommit takes them away as any other. */
KTEST(aspace_jit_wx_only_on_jit)
{
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    struct vmo *plain, *v = new_jit(4 * PG);
    KT_EQ(vmo_create(4 * PG, 0, &plain), OK);
    uint64_t a = 0x400000, b = 0x800000;
    KT_EQ(aspace_map(as, plain, 0, PG, RWX | CAN_ALL | ASPACE_FIXED, &a), ERR_INVALID_ARGS);
    KT_EQ(aspace_map(as, plain, 0, PG, RWX | CAN_ALL | ASPACE_FIXED | ASPACE_REPLACE, &a),
          ERR_INVALID_ARGS);
    KT_ASSERT(!aspace_maps_jit(as));
    /* A plain mapping whose maximum has write and execute (no handle gives
     * that, but the kernel may ask): still never both at once. */
    KT_EQ(aspace_map(as, plain, 0, 2 * PG, RW | CAN_ALL | ASPACE_FIXED, &a), OK);
    KT_EQ(aspace_protect(as, a, PG, RWX), ERR_INVALID_ARGS);
    KT_EQ(aspace_protect(as, a, 2 * PG, RX), OK);
    KT_EQ(aspace_protect(as, a, 2 * PG, RWX), ERR_INVALID_ARGS);
    KT_EQ(aspace_protect(as, 0x10000000, PG, RWX), ERR_INVALID_ARGS);   /* nothing there */

    KT_EQ(aspace_map(as, v, 0, 4 * PG, RWX | CAN_ALL | ASPACE_FIXED, &b), OK);
    KT_ASSERT(aspace_maps_jit(as));
    KT_EQ(aspace_fault(as, b, ASPACE_WRITE), OK);
    KT_ASSERT(pte_wx(as, b));
    KT_EQ(aspace_fault(as, b + PG, ASPACE_EXEC), OK);
    KT_ASSERT(pte_x(as, b + PG));
    KT_EQ(aspace_fault(as, b + PG, ASPACE_WRITE), OK);
    KT_ASSERT(pte_wx(as, b + PG));
    KT_EQ(vmo_page_phys(v, 0), aspace_pte(as, b) & 0x000ffffffffff000ull);
    /* A decommit clears them; the next touch commits a zero page. */
    KT_EQ(vmo_decommit(v, 0, PG), OK);
    KT_EQ(aspace_pte(as, b) & PTE_P, 0);
    KT_EQ(aspace_fault(as, b, ASPACE_EXEC), OK);
    KT_EQ(vmo_committed(v), 2 * PG);
    KT_EQ(aspace_unmap(as, b, 4 * PG), OK);
    /* The address space keeps its mark. */
    KT_ASSERT(aspace_maps_jit(as));
    KT_EQ(aspace_unmap(as, a, 2 * PG), OK);
    aspace_unref(as);
    put(plain);
    put(v);
}

/* aspace_protect on a JIT mapping: every transition within the maximum,
 * the entries following (write taken away at once and given back by the
 * next write fault, execute by NX), faults refused past the permissions;
 * a maximum without write (an execute-only handle's) or without execute
 * never gets the other; W+X over a range that also holds a plain mapping
 * is refused with nothing changed. */
KTEST(aspace_jit_protect)
{
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    struct vmo *v = new_jit(8 * PG), *plain;
    KT_EQ(vmo_create(PG, 0, &plain), OK);
    uint64_t a = 0x400000;
    KT_EQ(aspace_map(as, v, 0, 4 * PG, RWX | CAN_ALL | ASPACE_FIXED, &a), OK);
    KT_EQ(aspace_fault(as, a, ASPACE_WRITE), OK);
    KT_ASSERT(pte_wx(as, a));

    KT_EQ(aspace_protect(as, a, PG, RX), OK);
    KT_ASSERT(!(aspace_pte(as, a) & PTE_W) && pte_x(as, a));
    KT_EQ(aspace_fault(as, a, ASPACE_WRITE), ERR_ACCESS_DENIED);
    KT_EQ(aspace_fault(as, a, ASPACE_EXEC), OK);
    KT_EQ(aspace_protect(as, a, PG, RW), OK);
    KT_ASSERT((aspace_pte(as, a) & PTE_NX) || !cpu_features.nx);
    KT_EQ(aspace_fault(as, a, ASPACE_EXEC), ERR_ACCESS_DENIED);
    KT_EQ(aspace_fault(as, a, ASPACE_WRITE), OK);
    KT_EQ(aspace_protect(as, a, PG, RWX), OK);
    KT_EQ(aspace_fault(as, a, ASPACE_EXEC), OK);
    KT_EQ(aspace_fault(as, a, ASPACE_WRITE), OK);
    KT_ASSERT(pte_wx(as, a));
    KT_EQ(aspace_protect(as, a, PG, 0), OK);   /* no access: the entry goes */
    KT_EQ(aspace_pte(as, a) & PTE_P, 0);
    KT_EQ(aspace_fault(as, a, ASPACE_READ), ERR_ACCESS_DENIED);
    KT_EQ(aspace_protect(as, a, 4 * PG, RWX), OK);   /* and back: one mapping again */
    KT_EQ(aspace_mapping_count(as), 1);

    /* A plain mapping right after it: W+X over both is refused, nothing
     * changed (the JIT one stays RWX and whole). */
    uint64_t p = a + 4 * PG;
    KT_EQ(aspace_map(as, plain, 0, PG, RW | CAN_ALL | ASPACE_FIXED, &p), OK);
    KT_EQ(aspace_protect(as, a, 5 * PG, RWX), ERR_INVALID_ARGS);
    KT_EQ(aspace_protect(as, a + 3 * PG, 2 * PG, RWX), ERR_INVALID_ARGS);
    KT_EQ(aspace_mapping_count(as), 2);
    KT_EQ(aspace_fault(as, a + 3 * PG, ASPACE_WRITE), OK);
    KT_ASSERT(pte_wx(as, a + 3 * PG));
    KT_EQ(aspace_protect(as, a, 5 * PG, RX), OK);   /* not W+X: fine across both */
    KT_EQ(aspace_unmap(as, a, 5 * PG), OK);

    /* An execute-only maximum (read and execute: a handle without write). */
    uint64_t b = 0x800000;
    KT_EQ(aspace_map(as, v, 4 * PG, 2 * PG,
                     RX | ASPACE_CAN_READ | ASPACE_CAN_EXEC | ASPACE_FIXED, &b), OK);
    KT_EQ(aspace_protect(as, b, PG, RWX), ERR_ACCESS_DENIED);
    KT_EQ(aspace_protect(as, b, PG, RW), ERR_ACCESS_DENIED);
    KT_EQ(aspace_fault(as, b, ASPACE_WRITE), ERR_ACCESS_DENIED);
    KT_EQ(aspace_fault(as, b, ASPACE_EXEC), OK);
    KT_EQ(aspace_unmap(as, b, 2 * PG), OK);
    /* A write-only maximum (read and write: a handle without execute). */
    KT_EQ(aspace_map(as, v, 4 * PG, 2 * PG,
                     RW | ASPACE_CAN_READ | ASPACE_CAN_WRITE | ASPACE_FIXED, &b), OK);
    KT_EQ(aspace_protect(as, b, PG, RWX), ERR_ACCESS_DENIED);
    KT_EQ(aspace_protect(as, b, PG, RX), ERR_ACCESS_DENIED);
    KT_EQ(aspace_fault(as, b, ASPACE_EXEC), ERR_ACCESS_DENIED);
    KT_EQ(aspace_fault(as, b, ASPACE_WRITE), OK);
    KT_ASSERT((aspace_pte(as, b) & PTE_NX) || !cpu_features.nx);
    KT_EQ(aspace_unmap(as, b, 2 * PG), OK);
    aspace_unref(as);
    put(plain);
    put(v);
}

/* ---- the system calls ---------------------------------------------------------- */

/* A handle table with a root handle holding `powers`, a vmar and a fresh
 * VMO of `pages` pages (sys_vmo_create's rights). */
static void sys_setup(struct handle_table *t, rights_t powers, handle_t *root, handle_t *vmar,
                      uint64_t pages, handle_t *v)
{
    handle_table_init(t);
    struct kobject *r = resource_root();
    kobject_ref(r);
    struct khandle kr = khandle_from_new(r, RIGHTS_BASIC | powers);
    KT_EQ(handle_insert(t, &kr, root), OK);
    KT_EQ(sys_vmar_create(t, vmar), OK);
    KT_EQ(sys_vmo_create(t, pages * PG, 0, v), OK);
}

static rights_t rights_of(struct handle_table *t, handle_t h)
{
    struct kobject *obj;
    rights_t r = 0;
    KT_EQ(handle_get(t, h, OBJ_NONE, 0, &obj, &r), OK);
    kobject_unref(obj);
    return r;
}

static struct vmo *vmo_of(struct handle_table *t, handle_t h)
{
    struct kobject *obj;
    KT_EQ(handle_get(t, h, OBJ_VMO, 0, &obj, NULL), OK);
    struct vmo *v = vmo_from_kobject(obj);
    kobject_unref(obj);   /* the handle keeps it alive */
    return v;
}

#define VR (RIGHT_READ | RIGHT_MAP)

/* vmo_make_jit's refusals: no power, missing rights, a second handle, a
 * page, a mapping; each takes the handle (gone whatever the status) and
 * leaves the VMO plain. Then the call that works: RIGHT_EXEC added, and
 * nothing else changed. */
KTEST(vmo_jit_sys_make)
{
    struct handle_table t;
    handle_t root, vmar, h, x, h2, nopower;
    sys_setup(&t, RIGHT_ROOT_VMEX | RIGHT_ROOT_SYSINFO, &nopower, &vmar, 1, &h);
    struct vmo *v = vmo_of(&t, h);
    kobject_ref(vmo_kobject(v));
    KT_EQ(sys_vmo_make_jit(&t, h, nopower, &x), ERR_ACCESS_DENIED);
    KT_EQ(handle_close(&t, h), ERR_BAD_HANDLE);   /* gone */
    KT_ASSERT(!vmo_is_jit(v));
    put(v);
    handle_table_destroy(&t);

    sys_setup(&t, RIGHT_ROOT_JIT, &root, &vmar, 2, &h);
    /* A handle without write (or map) can't make one. */
    KT_EQ(handle_duplicate(&t, h, RIGHTS_BASIC | VR, &h2), OK);
    v = vmo_of(&t, h);
    KT_EQ(handle_close(&t, h), OK);
    KT_EQ(sys_vmo_make_jit(&t, h2, root, &x), ERR_ACCESS_DENIED);
    KT_EQ(handle_close(&t, h2), ERR_BAD_HANDLE);
    /* Two handles: refused, the one passed is gone, the other stays plain. */
    KT_EQ(sys_vmo_create(&t, 2 * PG, 0, &h), OK);
    KT_EQ(handle_duplicate(&t, h, RIGHT_SAME, &h2), OK);
    v = vmo_of(&t, h2);
    KT_EQ(sys_vmo_make_jit(&t, h, root, &x), ERR_BAD_STATE);
    KT_EQ(handle_close(&t, h), ERR_BAD_HANDLE);
    KT_ASSERT(!vmo_is_jit(v));
    /* A page in it. */
    KT_EQ(sys_vmo_write(&t, h2, 0, "a", 1), OK);
    KT_EQ(sys_vmo_make_jit(&t, h2, root, &x), ERR_BAD_STATE);
    /* A mapping. */
    uint64_t a = 0;
    KT_EQ(sys_vmo_create(&t, 2 * PG, 0, &h), OK);
    v = vmo_of(&t, h);
    kobject_ref(vmo_kobject(v));
    KT_EQ(sys_vmar_map(&t, vmar, h, 0, PG, VMAR_READ, &a), OK);
    KT_EQ(sys_vmo_make_jit(&t, h, root, &x), ERR_BAD_STATE);
    KT_ASSERT(!vmo_is_jit(v));
    KT_EQ(sys_vmar_unmap(&t, vmar, a, PG), OK);
    put(v);
    /* A kept VMO. */
    KT_EQ(sys_vmo_create(&t, PG, VMO_KEEP_PAGES, &h), OK);
    KT_EQ(sys_vmo_make_jit(&t, h, root, &x), ERR_NOT_SUPPORTED);
    /* Not a VMO: the value is closed all the same. */
    KT_EQ(sys_vmo_make_jit(&t, vmar, root, &x), ERR_WRONG_TYPE);
    KT_EQ(handle_close(&t, vmar), ERR_BAD_HANDLE);
    handle_table_destroy(&t);

    /* The call that works. */
    sys_setup(&t, RIGHT_ROOT_JIT, &root, &vmar, 2, &h);
    rights_t before = rights_of(&t, h);
    v = vmo_of(&t, h);
    KT_EQ(sys_vmo_make_jit(&t, h, root, &x), OK);
    KT_ASSERT(vmo_is_jit(v));
    KT_EQ(rights_of(&t, x), before | RIGHT_EXEC);
    KT_ASSERT(rights_of(&t, x) & RIGHT_TRANSFER);   /* delegable */
    KT_EQ(sys_vmo_make_jit(&t, x, root, &h), ERR_BAD_STATE);   /* JIT already; x gone */
    handle_table_destroy(&t);
}

/* Through handles: W+X needs a JIT VMO and both rights; a duplicate without
 * write maps execute-only and never writes, one without execute never runs;
 * a handle passed on to another table keeps exactly its rights; the old
 * ways to executable code refuse a JIT VMO. */
KTEST(vmo_jit_sys_handles)
{
    struct handle_table t, u;
    handle_t root, vmar, h, x;
    sys_setup(&t, RIGHT_ROOT_JIT, &root, &vmar, 4, &h);
    uint64_t a = 0;
    KT_EQ(sys_vmar_map(&t, vmar, h, 0, PG, VMAR_READ | VMAR_WRITE | VMAR_EXEC, &a),
          ERR_INVALID_ARGS);   /* plain: W^X whatever the rights */
    KT_EQ(sys_vmo_make_jit(&t, h, root, &x), OK);
    struct aspace *as = NULL;
    {
        struct kobject *o;
        KT_EQ(handle_get(&t, vmar, OBJ_VMAR, 0, &o, NULL), OK);
        as = vmar_aspace(vmar_from_kobject(o));
        kobject_unref(o);   /* the handle keeps it */
    }
    KT_EQ(sys_vmar_map(&t, vmar, x, 0, PG, VMAR_READ | VMAR_WRITE | VMAR_EXEC, &a), OK);
    KT_ASSERT(aspace_maps_jit(as));
    KT_EQ(aspace_fault(as, a, ASPACE_WRITE), OK);
    KT_ASSERT(pte_wx(as, a));
    KT_EQ(sys_vmar_protect(&t, vmar, a, PG, VMAR_READ | VMAR_EXEC), OK);
    KT_EQ(sys_vmar_protect(&t, vmar, a, PG, VMAR_READ | VMAR_WRITE | VMAR_EXEC), OK);

    /* Execute-only: no write, no resize. */
    handle_t xo, wo;
    KT_EQ(handle_duplicate(&t, x, RIGHTS_BASIC | VR | RIGHT_EXEC, &xo), OK);
    uint64_t b = 0;
    KT_EQ(sys_vmar_map(&t, vmar, xo, 0, PG, VMAR_READ | VMAR_WRITE | VMAR_EXEC, &b),
          ERR_ACCESS_DENIED);
    KT_EQ(sys_vmar_map(&t, vmar, xo, 0, PG, VMAR_READ | VMAR_WRITE, &b), ERR_ACCESS_DENIED);
    KT_EQ(sys_vmar_map(&t, vmar, xo, 0, PG, VMAR_READ | VMAR_EXEC, &b), OK);
    KT_EQ(sys_vmar_protect(&t, vmar, b, PG, VMAR_READ | VMAR_WRITE | VMAR_EXEC),
          ERR_ACCESS_DENIED);
    KT_EQ(sys_vmar_protect(&t, vmar, b, PG, VMAR_READ | VMAR_WRITE), ERR_ACCESS_DENIED);
    KT_EQ(aspace_fault(as, b, ASPACE_WRITE), ERR_ACCESS_DENIED);
    KT_EQ(aspace_fault(as, b, ASPACE_EXEC), OK);
    KT_EQ(aspace_pte(as, b) & 0x000ffffffffff000ull, aspace_pte(as, a) & 0x000ffffffffff000ull);
    KT_EQ(sys_vmo_write(&t, xo, 0, "a", 1), ERR_ACCESS_DENIED);
    KT_EQ(sys_vmo_decommit(&t, xo, 0, PG), ERR_ACCESS_DENIED);
    KT_EQ(sys_vmar_unmap(&t, vmar, b, PG), OK);
    /* Write without execute. */
    KT_EQ(handle_duplicate(&t, x, RIGHTS_BASIC | VR | RIGHT_WRITE, &wo), OK);
    KT_EQ(sys_vmar_map(&t, vmar, wo, PG, PG, VMAR_READ | VMAR_WRITE | VMAR_EXEC, &b),
          ERR_ACCESS_DENIED);
    KT_EQ(sys_vmar_map(&t, vmar, wo, PG, PG, VMAR_READ | VMAR_EXEC, &b), ERR_ACCESS_DENIED);
    KT_EQ(sys_vmar_map(&t, vmar, wo, PG, PG, VMAR_READ | VMAR_WRITE, &b), OK);
    KT_EQ(sys_vmar_protect(&t, vmar, b, PG, VMAR_READ | VMAR_EXEC), ERR_ACCESS_DENIED);
    KT_EQ(aspace_fault(as, b, ASPACE_EXEC), ERR_ACCESS_DENIED);
    KT_EQ(sys_vmo_write(&t, wo, PG, "b", 1), OK);
    KT_EQ(sys_vmar_unmap(&t, vmar, b, PG), OK);

    /* Passed on through a channel to another table: the execute-only one
     * arrives execute-only, and a narrowed send narrows it further. */
    handle_t c0, c1, got[2];
    KT_EQ(sys_channel_create(&t, &c0, &c1), OK);
    handle_table_init(&u);
    struct khandle kc;
    KT_EQ(handle_remove(&t, c1, &kc), OK);
    KT_EQ(handle_insert(&u, &kc, &c1), OK);
    rights_t xo_rights = rights_of(&t, xo);
    handle_t send[2] = { xo, wo };
    rights_t narrow[2] = { RIGHT_SAME, RIGHTS_BASIC | VR };
    KT_EQ(sys_channel_write_rights(&t, c0, "j", 1, send, narrow, 2), OK);
    uint32_t nb = 0, nh = 0;
    char byte;
    KT_EQ(sys_channel_read(&u, c1, &byte, 1, &nb, got, 2, &nh), OK);
    KT_EQ(nh, 2);
    KT_EQ(rights_of(&u, got[0]), xo_rights);
    KT_EQ(rights_of(&u, got[1]), RIGHTS_BASIC | VR);
    handle_t uvmar;
    KT_EQ(sys_vmar_create(&u, &uvmar), OK);
    uint64_t c = 0;
    KT_EQ(sys_vmar_map(&u, uvmar, got[0], 0, PG, VMAR_READ | VMAR_WRITE | VMAR_EXEC, &c),
          ERR_ACCESS_DENIED);
    KT_EQ(sys_vmar_map(&u, uvmar, got[0], 0, PG, VMAR_READ | VMAR_EXEC, &c), OK);
    KT_EQ(sys_vmo_write(&u, got[0], 0, "c", 1), ERR_ACCESS_DENIED);
    KT_EQ(sys_vmar_map(&u, uvmar, got[1], 0, PG, VMAR_READ | VMAR_EXEC, &c), ERR_ACCESS_DENIED);
    handle_table_destroy(&u);

    /* The old ways to code: a JIT VMO is never sealed (x is gone after). */
    handle_t s, child;
    KT_EQ(sys_vmo_create_child(&t, x, VMO_CHILD_SNAPSHOT, 0, PG, &child), ERR_NOT_SUPPORTED);
    struct kobject *vmex = resource_root();
    kobject_ref(vmex);
    struct khandle kv = khandle_from_new(vmex, RIGHTS_BASIC | RIGHT_ROOT_VMEX);
    handle_t vroot;
    KT_EQ(handle_insert(&t, &kv, &vroot), OK);
    KT_EQ(sys_vmar_unmap(&t, vmar, a, PG), OK);   /* (xo and wo went with the message) */
    KT_EQ(sys_vmo_make_exec(&t, x, vroot, &s), ERR_BAD_STATE);   /* one handle, unmapped */
    KT_EQ(handle_close(&t, x), ERR_BAD_HANDLE);
    handle_table_destroy(&t);
}

/* ---- hostile (CR1, docs/CR-REVIEW-KERNEL.md) ---------------------------------- */

/* The aspace a vmar handle names (the handle keeps it alive). */
static struct aspace *aspace_of(struct handle_table *t, handle_t vmar)
{
    struct kobject *o;
    KT_EQ(handle_get(t, vmar, OBJ_VMAR, 0, &o, NULL), OK);
    struct aspace *as = vmar_aspace(vmar_from_kobject(o));
    kobject_unref(o);
    return as;
}

#define NOJIT (RIGHTS_ROOT & ~RIGHT_ROOT_JIT)

/* A program with every power but the JIT one tries each way to hold one
 * VMO's pages writable and executable at once: a second mapping through
 * the same handle, a handle that can't be had, the old way to code (it
 * seals: nothing writes it again), a copy-on-write child of approved code
 * (a fresh VMO's rights: no execute). Every way is refused, and the
 * address space is never marked. */
KTEST(vmo_jit_hostile_alias)
{
    struct handle_table t;
    handle_t root, vmar, h, h2, x, c, d;
    sys_setup(&t, NOJIT, &root, &vmar, 2, &h);
    struct aspace *as = aspace_of(&t, vmar);
    uint64_t a = 0, b = 0;
    KT_EQ(sys_vmar_map(&t, vmar, h, 0, PG, VMAR_READ | VMAR_WRITE, &a), OK);
    KT_EQ(sys_vmo_write(&t, h, 0, "\xc3", 1), OK);   /* a `ret`, written */
    KT_EQ(sys_vmar_map(&t, vmar, h, 0, PG, VMAR_READ | VMAR_EXEC, &b), ERR_ACCESS_DENIED);
    KT_EQ(sys_vmar_map(&t, vmar, h, 0, PG, VMAR_READ | VMAR_WRITE | VMAR_EXEC, &b),
          ERR_INVALID_ARGS);
    KT_EQ(sys_vmar_protect(&t, vmar, a, PG, VMAR_READ | VMAR_EXEC), ERR_ACCESS_DENIED);
    KT_EQ(sys_vmar_protect(&t, vmar, a, PG, VMAR_READ | VMAR_WRITE | VMAR_EXEC), ERR_INVALID_ARGS);
    /* No power: the call takes the handle and changes nothing. */
    KT_EQ(handle_duplicate(&t, h, RIGHT_SAME, &h2), OK);
    struct vmo *v = vmo_of(&t, h2);
    KT_EQ(sys_vmo_make_jit(&t, h, root, &x), ERR_ACCESS_DENIED);
    KT_EQ(handle_close(&t, h), ERR_BAD_HANDLE);
    KT_EQ(sys_vmo_make_jit(&t, h2, h2, &x), ERR_WRONG_TYPE);   /* (a VMO is no root) */
    KT_ASSERT(!vmo_is_jit(v));
    /* The old way: sealed first, so nothing ever writes it again. */
    KT_EQ(sys_vmo_create(&t, PG, 0, &h), OK);
    KT_EQ(sys_vmar_map(&t, vmar, h, 0, PG, VMAR_READ | VMAR_WRITE, &b), OK);
    KT_EQ(sys_vmo_make_exec(&t, h, root, &x), ERR_BAD_STATE);   /* mapped writable */
    KT_EQ(sys_vmar_unmap(&t, vmar, b, PG), OK);
    KT_EQ(sys_vmo_create(&t, PG, 0, &h), OK);
    KT_EQ(sys_vmo_write(&t, h, 0, "\xc3", 1), OK);
    KT_EQ(sys_vmo_make_exec(&t, h, root, &x), OK);
    KT_EQ(rights_of(&t, x) & (RIGHT_WRITE | RIGHT_EXEC), RIGHT_EXEC);
    KT_EQ(sys_vmar_map(&t, vmar, x, 0, PG, VMAR_READ | VMAR_EXEC, &b), OK);
    uint64_t w = 0;
    KT_EQ(sys_vmar_map(&t, vmar, x, 0, PG, VMAR_READ | VMAR_WRITE, &w), ERR_ACCESS_DENIED);
    KT_EQ(sys_vmar_protect(&t, vmar, b, PG, VMAR_READ | VMAR_WRITE | VMAR_EXEC), ERR_INVALID_ARGS);
    KT_EQ(sys_vmo_write(&t, x, 0, "\x90", 1), ERR_ACCESS_DENIED);
    KT_EQ(sys_vmo_decommit(&t, x, 0, PG), ERR_ACCESS_DENIED);
    /* Its copy-on-write child is a fresh VMO: writable, never executable,
     * and a write to it leaves the approved page as it was. */
    KT_EQ(sys_vmo_create_child(&t, x, VMO_CHILD_SNAPSHOT, 0, PG, &c), OK);
    KT_EQ(rights_of(&t, c) & RIGHT_EXEC, 0);
    KT_EQ(sys_vmar_map(&t, vmar, c, 0, PG, VMAR_READ | VMAR_EXEC, &w), ERR_ACCESS_DENIED);
    KT_EQ(sys_vmo_write(&t, c, 0, "\x90", 1), OK);
    char byte = 0;
    KT_EQ(sys_vmo_read(&t, x, 0, &byte, 1), OK);
    KT_EQ((uint8_t)byte, 0xc3);
    /* The child can't become code either way while it is mapped or shared. */
    KT_EQ(handle_duplicate(&t, c, RIGHT_SAME, &d), OK);
    KT_EQ(sys_vmo_make_exec(&t, c, root, &x), ERR_BAD_STATE);   /* d is another handle */
    KT_EQ(sys_vmo_make_jit(&t, d, root, &x), ERR_ACCESS_DENIED);
    KT_ASSERT(!aspace_maps_jit(as));
    KT_EQ(aspace_check(as), 2);   /* a's RW page and b's RX one; W^X entry by entry */
    handle_table_destroy(&t);
}

/* A program with the power hands JIT memory to one without it (its own
 * choice: handles are delegable), narrowed so it can't be passed on. The
 * receiver gets exactly that: W+X on that VMO alone (and is marked), no
 * way to pass it on, no JIT memory of its own, no plain copy to keep or
 * seal, W^X everywhere else. An execute-only duplicate never writes. */
KTEST(vmo_jit_hostile_handoff)
{
    struct handle_table t, u;
    handle_t root, vmar, h, x;
    sys_setup(&t, RIGHT_ROOT_JIT, &root, &vmar, 2, &h);
    KT_EQ(sys_vmo_make_jit(&t, h, root, &x), OK);
    handle_t uroot, uvmar, uplain;
    sys_setup(&u, NOJIT, &uroot, &uvmar, 1, &uplain);
    struct aspace *uas = aspace_of(&u, uvmar);

    handle_t c0, c1, xs[2], got[2];
    KT_EQ(sys_channel_create(&t, &c0, &c1), OK);
    struct khandle kc;
    KT_EQ(handle_remove(&t, c1, &kc), OK);
    KT_EQ(handle_insert(&u, &kc, &c1), OK);
    KT_EQ(handle_duplicate(&t, x, RIGHT_SAME, &xs[0]), OK);
    KT_EQ(handle_duplicate(&t, x, RIGHT_SAME, &xs[1]), OK);
    const rights_t kept = RIGHT_DUPLICATE | RIGHT_WAIT | RIGHT_INSPECT | VR;
    rights_t narrow[2] = { kept | RIGHT_WRITE | RIGHT_EXEC, kept | RIGHT_EXEC };
    KT_EQ(sys_channel_write_rights(&t, c0, "j", 1, xs, narrow, 2), OK);
    uint32_t nb = 0, nh = 0;
    char byte;
    KT_EQ(sys_channel_read(&u, c1, &byte, 1, &nb, got, 2, &nh), OK);
    KT_EQ(nh, 2);
    KT_EQ(rights_of(&u, got[0]), narrow[0]);

    /* W+X on it, as given; marked. */
    uint64_t a = 0, b = 0;
    KT_ASSERT(!aspace_maps_jit(uas));
    KT_EQ(sys_vmar_map(&u, uvmar, got[0], 0, PG, VMAR_READ | VMAR_WRITE | VMAR_EXEC, &a), OK);
    KT_ASSERT(aspace_maps_jit(uas));
    KT_EQ(aspace_fault(uas, a, ASPACE_WRITE), OK);
    KT_ASSERT(pte_wx(uas, a));
    /* No passing it on (no transfer), no JIT memory of its own (no power,
     * and the handle given is no root), W^X on its own VMOs. */
    handle_t d0, d1, again;
    KT_EQ(sys_channel_create(&u, &d0, &d1), OK);
    KT_EQ(handle_duplicate(&u, got[0], RIGHT_SAME, &again), OK);   /* a copy: no transfer either */
    KT_EQ(sys_channel_write_rights(&u, d0, "k", 1, &again, (rights_t[]){ RIGHT_SAME }, 1),
          ERR_ACCESS_DENIED);
    KT_EQ(handle_close(&u, again), ERR_BAD_HANDLE);   /* (a refused send closes it) */
    KT_EQ(sys_vmo_make_jit(&u, uplain, uroot, &h), ERR_ACCESS_DENIED);
    KT_EQ(sys_vmo_create(&u, PG, 0, &uplain), OK);
    KT_EQ(sys_vmo_make_jit(&u, uplain, got[0], &h), ERR_WRONG_TYPE);
    KT_EQ(sys_vmo_create(&u, PG, 0, &uplain), OK);
    KT_EQ(sys_vmar_map(&u, uvmar, uplain, 0, PG, VMAR_READ | VMAR_WRITE | VMAR_EXEC, &b),
          ERR_INVALID_ARGS);
    /* No plain copy of the JIT memory to keep writing or to seal. */
    handle_t kid;
    KT_EQ(sys_vmo_create_child(&u, got[0], VMO_CHILD_SNAPSHOT, 0, PG, &kid), ERR_NOT_SUPPORTED);
    KT_EQ(sys_vmo_make_exec(&u, got[0], uroot, &h), ERR_BAD_STATE);   /* never sealed; gone */
    KT_EQ(sys_vmar_protect(&u, uvmar, a, PG, VMAR_READ | VMAR_EXEC), OK);   /* still its mapping */
    /* The execute-only one: never writes, by any route. */
    KT_EQ(sys_vmar_map(&u, uvmar, got[1], 0, PG, VMAR_READ | VMAR_WRITE, &b), ERR_ACCESS_DENIED);
    KT_EQ(sys_vmar_map(&u, uvmar, got[1], 0, PG, VMAR_READ | VMAR_EXEC, &b), OK);
    KT_EQ(sys_vmar_protect(&u, uvmar, b, PG, VMAR_READ | VMAR_WRITE | VMAR_EXEC),
          ERR_ACCESS_DENIED);
    KT_EQ(sys_vmo_write(&u, got[1], 0, "a", 1), ERR_ACCESS_DENIED);
    KT_EQ(sys_vmo_set_size(&u, got[1], 0), ERR_ACCESS_DENIED);
    KT_EQ(aspace_fault(uas, b, ASPACE_WRITE), ERR_ACCESS_DENIED);
    KT_EQ(aspace_check(uas), 2);
    handle_table_destroy(&u);
    handle_table_destroy(&t);
}

/* W^X across a spawn: a parent holds the vmar of a child's address space
 * (as spawn does) and maps into it. Without JIT memory nothing it maps
 * there is W+X; JIT memory it maps there marks the child, which runs it
 * only through the parent's mapping; a fork-like copy-on-write snapshot of
 * JIT memory doesn't exist, and one of plain memory never executes. */
KTEST(vmo_jit_hostile_spawn)
{
    struct handle_table t;
    handle_t root, vmar, h, x, cvmar, plain, snap;
    sys_setup(&t, RIGHT_ROOT_JIT, &root, &vmar, 2, &h);
    KT_EQ(sys_vmar_create(&t, &cvmar), OK);   /* the child's address space */
    struct aspace *child = aspace_of(&t, cvmar), *mine = aspace_of(&t, vmar);
    KT_EQ(sys_vmo_create(&t, 2 * PG, 0, &plain), OK);
    uint64_t a = 0, b = 0, c = 0;
    KT_EQ(sys_vmar_map(&t, cvmar, plain, 0, PG, VMAR_READ | VMAR_WRITE | VMAR_EXEC, &a),
          ERR_INVALID_ARGS);
    KT_EQ(sys_vmar_map(&t, cvmar, plain, 0, PG, VMAR_READ | VMAR_WRITE, &a), OK);
    KT_EQ(sys_vmar_protect(&t, cvmar, a, PG, VMAR_READ | VMAR_WRITE | VMAR_EXEC), ERR_INVALID_ARGS);
    KT_ASSERT(!aspace_maps_jit(child));
    /* A fork-like copy of the plain memory: writable, never executable. */
    KT_EQ(sys_vmo_create_child(&t, plain, VMO_CHILD_SNAPSHOT, 0, 2 * PG, &snap), OK);
    KT_EQ(sys_vmar_map(&t, cvmar, snap, 0, PG, VMAR_READ | VMAR_EXEC, &b), ERR_ACCESS_DENIED);

    KT_EQ(sys_vmo_make_jit(&t, h, root, &x), OK);
    KT_EQ(sys_vmar_map(&t, vmar, x, 0, PG, VMAR_READ | VMAR_WRITE | VMAR_EXEC, &b), OK);
    KT_EQ(sys_vmar_map(&t, cvmar, x, 0, PG, VMAR_READ | VMAR_WRITE | VMAR_EXEC, &c), OK);
    KT_ASSERT(aspace_maps_jit(child));
    KT_EQ(aspace_fault(child, c, ASPACE_WRITE), OK);
    KT_EQ(aspace_fault(mine, b, ASPACE_EXEC), OK);
    KT_ASSERT(pte_wx(child, c));
    /* Each address space's mapping is its own: the parent takes write off
     * its own, and the child's stays as the parent made it (until the
     * parent, holding the child's vmar, changes that too). */
    KT_EQ(sys_vmar_protect(&t, vmar, b, PG, VMAR_READ | VMAR_EXEC), OK);
    KT_ASSERT(pte_wx(child, c));
    KT_EQ(sys_vmar_protect(&t, cvmar, c, PG, VMAR_READ | VMAR_EXEC), OK);
    KT_ASSERT(!pte_wx(child, c));
    /* No fork-like copy of JIT memory at all. */
    handle_t jsnap;
    KT_EQ(sys_vmo_create_child(&t, x, VMO_CHILD_SNAPSHOT, 0, PG, &jsnap), ERR_NOT_SUPPORTED);
    KT_EQ(sys_vmo_create_child(&t, x, VMO_CHILD_SNAPSHOT_AT_LEAST_ON_WRITE, 0, PG, &jsnap),
          ERR_NOT_SUPPORTED);
    KT_EQ(aspace_check(child), 2);
    KT_EQ(aspace_check(mine), 1);
    handle_table_destroy(&t);
}

/* ---- races --------------------------------------------------------------------- */

/* At the moment vmo_make_jit has taken the handle out and is about to
 * check (the hook), a mapping appears through a reference taken before
 * (another thread's vmar_map past its handle_get): the marking must see
 * it under the lock and refuse, so no mapping made before the VMO became
 * JIT memory ever exists on one. */
static struct vmo *jr_vmo;
static struct aspace *jr_as;
static uint64_t jr_addr;
static status_t jr_map_st;

static void jr_hook(void *arg)
{
    if (arg != jr_vmo)
        return;
    jr_addr = 0;
    jr_map_st = aspace_map(jr_as, jr_vmo, 0, PG, R | ASPACE_CAN_READ, &jr_addr);
}

KTEST(vmo_jit_sys_make_races_map)
{
    struct handle_table t;
    handle_t root, vmar, h, x;
    sys_setup(&t, RIGHT_ROOT_JIT, &root, &vmar, 1, &h);
    jr_vmo = vmo_of(&t, h);
    kobject_ref(vmo_kobject(jr_vmo));
    KT_EQ(aspace_create(&jr_as), OK);
    jr_map_st = ERR_INTERNAL;
    __atomic_store_n(&dbg_hooks[DBG_VMO_EXEC_CHECK], jr_hook, __ATOMIC_RELEASE);
    status_t st = sys_vmo_make_jit(&t, h, root, &x);
    __atomic_store_n(&dbg_hooks[DBG_VMO_EXEC_CHECK], NULL, __ATOMIC_RELEASE);
    KT_EQ(jr_map_st, OK);
    KT_EQ(st, ERR_BAD_STATE);
    KT_ASSERT(!vmo_is_jit(jr_vmo));
    KT_EQ(aspace_unmap(jr_as, jr_addr, PG), OK);
    aspace_unref(jr_as);
    put(jr_vmo);
    handle_table_destroy(&t);
}
