/* Interrupt dispatch, and the per-CPU device vector allocator.
 *
 * Device vectors 0x31-0xef are allocated per (cpu, vector): an MSI names
 * one CPU's APIC ID and one vector, so the same vector number can belong
 * to a different owner on every CPU. Each CPU has a table of slots
 * (fn, ctx); irq_dispatch looks the slot up on the CPU the interrupt
 * arrived on, calls fn(ctx) with interrupts off, then sends the EOI.
 *
 * vector_free must not return while a handler for the slot is still
 * running (the caller frees ctx next). Each slot has a `busy` count that
 * the dispatcher raises BEFORE it reads fn and drops after fn returns;
 * vector_free clears fn and THEN reads busy, both sequentially consistent
 * (a locked RMW / xchg on x86), so one of the two always sees the other:
 * either the dispatcher reads fn == NULL and never calls it, or
 * vector_free sees busy != 0 and spins until the handler is done. The slot
 * is only handed out again after that, so a new owner's fn is never
 * paired with the old owner's ctx. vector_free spins rather than sleeps:
 * handlers are short, and it may run where sleeping is not allowed
 * (object destroy runs with preemption off, object.c td_run).
 *
 * A message the device posted before it was disabled can still land after
 * vector_free: it finds no owner and is counted (irq_device_unowned). To
 * make it unlikely that such a straggler hits a NEW owner of the same
 * vector, each CPU hands vectors out next-fit (a cursor that wraps), so a
 * freed vector is reused last. */
#include <jam/cpu.h>
#include <jam/interrupt.h>
#include <jam/irq.h>
#include <jam/lapic.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/percpu.h>
#include <jam/spinlock.h>
#include <jam/time.h>
#include <jam/trap.h>
#include <jam/x86.h>

static irq_handler_t handlers[256];
uint64_t irq_unexpected;
uint8_t irq_last_unexpected;
uint64_t irq_device_unowned;
uint8_t irq_device_last_unowned;

_Static_assert(VEC_DEVICE_FIRST == VEC_COM1 + 1, "device vectors start after COM1");
_Static_assert(VEC_DEVICE_LAST < VEC_TIMER, "device vectors end below the timer");
_Static_assert(VEC_DEVICE_COUNT == VEC_DEVICE_LAST - VEC_DEVICE_FIRST + 1, "vector count");

struct vec_slot {
    vector_fn_t       fn;      /* NULL: unowned (atomic) */
    void             *ctx;     /* written before fn is published */
    uint32_t          busy;    /* handlers running now (only this CPU runs them) */
    bool              used;    /* allocated, possibly still draining (alloc_lock) */
};

struct vec_table {
    struct vec_slot s[VEC_DEVICE_COUNT];  /* one per device vector */
    uint16_t        count;                /* used slots (alloc_lock) */
    uint16_t        cursor;               /* next-fit start (alloc_lock) */
};

static struct vec_table *vtab[MAX_CPUS];
static spinlock_t alloc_lock = SPINLOCK_INIT("vector alloc");   /* a leaf */

void irq_register(uint8_t vector, irq_handler_t fn)
{
    handlers[vector] = fn;
}

/* Interrupts are off: this CPU's slot for v, if it has an owner. */
static bool device_dispatch(uint8_t v)
{
    struct vec_table *t = __atomic_load_n(&vtab[this_cpu()->index], __ATOMIC_ACQUIRE);
    if (!t)
        return false;
    struct vec_slot *s = &t->s[v - VEC_DEVICE_FIRST];
    __atomic_add_fetch(&s->busy, 1, __ATOMIC_SEQ_CST);
    vector_fn_t fn = __atomic_load_n(&s->fn, __ATOMIC_SEQ_CST);
    if (fn)
        fn(s->ctx);
    __atomic_sub_fetch(&s->busy, 1, __ATOMIC_RELEASE);
    return fn != NULL;
}

void irq_dispatch(struct trap_frame *f)
{
    uint8_t v = (uint8_t)f->vector;
    if (handlers[v]) {
        handlers[v](f);   /* handlers send their own EOI */
        return;
    }
    if (v >= VEC_DEVICE_FIRST && v <= VEC_DEVICE_LAST) {
        if (!device_dispatch(v)) {
            __atomic_store_n(&irq_device_last_unowned, v, __ATOMIC_RELAXED);
            __atomic_add_fetch(&irq_device_unowned, 1, __ATOMIC_RELAXED);
        }
        lapic_eoi();
        return;
    }
    if (v >= VEC_PIC_BASE && v < VEC_PIC_BASE + 16)
        return;   /* spurious 8259 interrupt: no EOI */
    /* Counted, not logged: see lapic.c. */
    __atomic_store_n(&irq_last_unexpected, v, __ATOMIC_RELAXED);
    __atomic_add_fetch(&irq_unexpected, 1, __ATOMIC_RELAXED);
    lapic_eoi();
}

/* ---- the allocator ------------------------------------------------------- */

uint32_t vector_pick_cpu(const struct vector_cpu_view *v, uint32_t n)
{
    uint32_t usable = 0;
    for (uint32_t i = 0; i < n; i++)
        usable += v[i].usable;
    uint32_t best = UINT32_MAX;
    for (uint32_t i = 0; i < n; i++) {
        if (!v[i].usable || v[i].nvec >= VEC_DEVICE_COUNT)
            continue;
        if (i == 0 && usable > 1)
            continue;   /* CPU 0 only when it is alone */
        if (best == UINT32_MAX) {
            best = i;
            continue;
        }
        bool e = v[i].type == CORE_EFFICIENCY, be = v[best].type == CORE_EFFICIENCY;
        if (e != be) {
            if (e)
                best = i;
            continue;
        }
        if (v[i].nvec < v[best].nvec)
            best = i;
    }
    return best;
}

/* xAPIC MSI destination: 8 bits, and 0xff is the broadcast ID. */
static bool apic_msi_ok(uint32_t apic_id)
{
    return apic_id < 0xff;
}

/* Every online CPU gets its table once, outside the lock (kzalloc may take
 * other locks). Tables are never freed. */
static status_t tables_ensure(void)
{
    for (uint32_t i = 0; i < cpu_count; i++) {
        if (__atomic_load_n(&vtab[i], __ATOMIC_ACQUIRE) || !cpus[i] || !cpus[i]->online)
            continue;
        struct vec_table *t = kzalloc(sizeof(*t));
        if (!t)
            return ERR_NO_MEMORY;
        t->cursor = VEC_DEVICE_COUNT - 1;   /* first allocation takes 0x31 */
        struct vec_table *none = NULL;
        if (!__atomic_compare_exchange_n(&vtab[i], &none, t, false, __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE))
            kfree(t);
    }
    return OK;
}

status_t vector_alloc(vector_fn_t fn, void *ctx, uint32_t *cpu, uint8_t *vec)
{
    if (!fn)
        return ERR_INVALID_ARGS;
    status_t st = tables_ensure();
    if (st != OK)
        return st;
    static struct vector_cpu_view view[MAX_CPUS];   /* alloc_lock */
    uint64_t f = spin_lock_irqsave(&alloc_lock);
    uint32_t n = cpu_count;
    for (uint32_t i = 0; i < n; i++) {
        struct cpu *c = cpus[i];
        struct vec_table *t = vtab[i];
        view[i] = (struct vector_cpu_view){
            .type = c ? (uint8_t)c->type : CORE_UNKNOWN,
            .usable = c && c->online && t && apic_msi_ok(c->lapic_id),
            .nvec = t ? t->count : 0,
        };
    }
    uint32_t pick = vector_pick_cpu(view, n);
    if (pick == UINT32_MAX) {
        spin_unlock_irqrestore(&alloc_lock, f);
        return ERR_NO_RESOURCES;
    }
    struct vec_table *t = vtab[pick];
    uint32_t idx = t->cursor;
    for (uint32_t k = 0; k < VEC_DEVICE_COUNT; k++) {
        idx = idx + 1 == VEC_DEVICE_COUNT ? 0 : idx + 1;
        if (!t->s[idx].used)
            break;
    }
    struct vec_slot *s = &t->s[idx];
    ASSERT(!s->used);   /* count < VEC_DEVICE_COUNT, so a free slot exists */
    s->used = true;
    s->ctx = ctx;
    __atomic_store_n(&s->fn, fn, __ATOMIC_RELEASE);   /* publishes ctx */
    t->count++;
    t->cursor = (uint16_t)idx;
    spin_unlock_irqrestore(&alloc_lock, f);
    *cpu = pick;
    *vec = (uint8_t)(VEC_DEVICE_FIRST + idx);
    return OK;
}

void vector_free(uint32_t cpu, uint8_t vec)
{
    if (cpu >= cpu_count || !vtab[cpu] || vec < VEC_DEVICE_FIRST || vec > VEC_DEVICE_LAST)
        panic("vector_free: bad vector %u:%#x", cpu, vec);
    struct vec_slot *s = &vtab[cpu]->s[vec - VEC_DEVICE_FIRST];
    uint64_t f = spin_lock_irqsave(&alloc_lock);
    if (!s->used || !s->fn)
        panic("vector_free: %u:%#x is not allocated", cpu, vec);
    __atomic_store_n(&s->fn, NULL, __ATOMIC_SEQ_CST);   /* then read busy: see the top */
    spin_unlock_irqrestore(&alloc_lock, f);

    /* A handler that saw fn before the store may still be running on cpu. */
    uint64_t start = rdtsc();
    while (__atomic_load_n(&s->busy, __ATOMIC_SEQ_CST)) {
        cpu_relax();
        if (rdtsc() - start > tsc_hz * 5)
            panic("vector_free: cpu %u still in the handler for %#x after 5 s", cpu, vec);
    }

    f = spin_lock_irqsave(&alloc_lock);
    s->ctx = NULL;
    s->used = false;
    vtab[cpu]->count--;
    spin_unlock_irqrestore(&alloc_lock, f);
}

uint32_t vector_count(uint32_t cpu)
{
    if (cpu >= MAX_CPUS)
        return 0;
    struct vec_table *t = __atomic_load_n(&vtab[cpu], __ATOMIC_ACQUIRE);
    return t ? __atomic_load_n(&t->count, __ATOMIC_RELAXED) : 0;
}

/* Fixed delivery, edge, physical destination mode, no redirection hint:
 * address 0xfee00000 | dest << 12, data = the vector. */
uint64_t msi_address(uint32_t cpu)
{
    ASSERT(cpu < cpu_count && cpus[cpu] && apic_msi_ok(cpus[cpu]->lapic_id));
    return 0xfee00000ull | (uint64_t)cpus[cpu]->lapic_id << 12;
}

uint32_t msi_data(uint8_t vec)
{
    return vec;
}
