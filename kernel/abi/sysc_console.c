/* System calls 110-118: what the console and the shell need
 * from the kernel. The objects are in <jam/console_svc.h>; the rules every
 * sysc_* follows are in sysc.h.
 *
 * Rights, all on a RES_ROOT handle (sysinfo_check_root; init hands each
 * service the root with only the ones it uses, none can map or slice):
 *   klog_open         RIGHT_ROOT_KLOG
 *   framebuffer_take  RIGHT_WRITE
 *   debug_command     RIGHT_ROOT_DEBUG
 *   reboot            RIGHT_ROOT_REBOOT
 *   serial_open       RIGHT_ROOT_SERIAL
 *   serial_write      RIGHT_WRITE
 * and on the objects: klog_read, klog_name and serial_read need RIGHT_READ.
 *
 * Each new object costs its creator's job one JOB_LIMIT_HANDLES unit
 * (like a resource), on top of the handle slot. */
#include <jam/console_svc.h>
#include <jam/fbcon.h>
#include <jam/kexec.h>
#include <jam/klog.h>
#include <jam/kprintf.h>
#include <jam/list.h>
#include <jam/mm.h>
#include <jam/process.h>
#include <jam/resource.h>
#include <jam/serial.h>
#include <jam/spinlock.h>
#include <jam/syscall_impl.h>
#include <jam/sysinfo.h>
#include <jam/vmo.h>
#include "sysc.h"

#define READER_RIGHTS (RIGHTS_BASIC | RIGHT_READ)
#define SERIAL_RIGHTS (RIGHTS_BASIC | RIGHT_READ)
#define SCREEN_RIGHTS RIGHTS_BASIC
#define FB_VMO_RIGHTS (RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE | RIGHT_MAP)
#define CHUNK         512     /* bytes copied per step (on the stack) */
#define KLOG_READ_MAX KLOG_SIZE   /* one read: the whole ring at most */
#define SERIAL_IO_MAX 4096
#define KLOG_NAME_MAX 32      /* klog_name: what kexec keeps (KEXEC_NAME) */

/* A charged small object: its job gets one handle unit back on destroy. */
static status_t charge(struct job *job)
{
    status_t st = job_charge(job, JOB_LIMIT_HANDLES, 1);
    if (st == OK)
        job_ref(job);
    return st;
}

static void uncharge(struct job *job)
{
    job_uncharge(job, JOB_LIMIT_HANDLES, 1);
    job_unref(job);
}

/* ---- kernel log readers -------------------------------------------------- */

struct klog_reader {
    struct kobject   base;    /* OBJ_KLOG */
    struct job      *job;     /* charged one handle unit (a reference) */
    uint64_t         seen;    /* end of the last read: written under the object lock,
                                 read by the tick (atomic) */
    uint64_t         keep;    /* bytes of the ring it sees: KLOG_SIZE, less for a test */
    struct list_node node;    /* on `readers` (readers_lock) */
};

static struct list_node readers = LIST_INIT(readers);
static spinlock_t readers_lock = SPINLOCK_INIT("klog readers");
static uint64_t signaled_head;   /* CPU 0's tick only */

static void reader_destroy(struct kobject *obj)
{
    struct klog_reader *r = (struct klog_reader *)obj;
    uint64_t f = spin_lock_irqsave(&readers_lock);
    list_del(&r->node);
    spin_unlock_irqrestore(&readers_lock, f);
    uncharge(r->job);
    kfree(r);
}

static const struct kobject_ops reader_ops = {
    .name = "klog",
    .destroy = reader_destroy,
};

status_t klog_reader_create(struct job *job, struct kobject **out)
{
    struct klog_reader *r = kzalloc(sizeof(*r));
    if (!r)
        return ERR_NO_MEMORY;
    status_t st = charge(job);
    if (st != OK) {
        kfree(r);
        return st;
    }
    r->job = job;
    r->keep = KLOG_SIZE;
    /* A new reader has read nothing: readable at once if the log has text. */
    kobject_init(&r->base, OBJ_KLOG, &reader_ops, "klog", klog_head() ? SIG_READABLE : 0);
    uint64_t f = spin_lock_irqsave(&readers_lock);
    list_add_tail(&readers, &r->node);
    spin_unlock_irqrestore(&readers_lock, f);
    *out = &r->base;
    return OK;
}

/* After a read that ended at `end`: readable iff the log is past it. The
 * check and the change are under the reader's lock, which the tick's
 * signal also takes, so a line written meanwhile is never lost: either the
 * head we look at includes it, or the tick sees it later (it compares the
 * head with what it last signalled). */
static void reader_update(struct klog_reader *r, uint64_t end)
{
    uint64_t f = spin_lock_irqsave(&r->base.lock);
    __atomic_store_n(&r->seen, end, __ATOMIC_RELAXED);
    if (end >= klog_head())
        kobject_signal_locked(&r->base, SIG_READABLE, 0);
    else
        kobject_signal_locked(&r->base, 0, SIG_READABLE);
    spin_unlock_irqrestore(&r->base.lock, f);
}

size_t klog_reader_read(struct kobject *reader, uint64_t pos, char *buf, size_t cap,
                        uint64_t *first)
{
    struct klog_reader *r = (struct klog_reader *)reader;
    size_t n = klog_read_kept(pos, r->keep, buf, cap, first);
    reader_update(r, *first + n);
    return n;
}

status_t klog_reader_read_to(struct kobject *reader, uint64_t pos, uint64_t cap, klog_sink_t sink,
                             void *ctx, uint64_t *first, uint64_t *done_out)
{
    struct klog_reader *r = (struct klog_reader *)reader;
    char chunk[CHUNK];
    uint64_t start = 0, done = 0;
    status_t st = OK;
    do {
        uint64_t f;
        size_t want = cap - done < CHUNK ? cap - done : CHUNK;
        size_t n = klog_read_kept(pos, r->keep, chunk, want, &f);
        if (done == 0)
            start = f;
        else if (f != pos)
            break;   /* overwritten between two steps: stop at the gap */
        if (n && (st = sink(ctx, done, chunk, n)) != OK)
            break;
        done += n;
        pos = f + n;
        if (n < want)
            break;
    } while (done < cap);
    /* Readable from where the caller's text ends, not from a step past a
     * gap that was read and thrown away. */
    reader_update(r, start + done);
    *first = start;
    *done_out = done;
    return st;
}

#ifndef JAM_NO_KTESTS
void klog_reader_test_keep(struct kobject *reader, uint64_t keep)
{
    ((struct klog_reader *)reader)->keep = keep < KLOG_SIZE ? keep : KLOG_SIZE;
}
#endif

void klog_poll(void)
{
    uint64_t h = klog_head();
    if (h == signaled_head)
        return;
    spin_lock(&readers_lock);   /* CPU 0's tick: interrupts are off */
    for (struct list_node *n = readers.next; n != &readers; n = n->next) {
        struct klog_reader *r = container_of(n, struct klog_reader, node);
        if (__atomic_load_n(&r->seen, __ATOMIC_RELAXED) < h)
            kobject_signal(&r->base, 0, SIG_READABLE);
    }
    spin_unlock(&readers_lock);
    signaled_head = h;
}

/* ---- COM1 input ----------------------------------------------------------- */

struct serial_in {
    struct kobject base;      /* OBJ_SERIAL */
    struct job    *job;       /* charged one handle unit (a reference) */
    bool           started;   /* serial_rx_start succeeded: stop on zero handles */
};

static void serial_in_notify(void *ctx)
{
    struct serial_in *s = ctx;
    kobject_signal(&s->base, 0, SIG_READABLE);
}

static void serial_in_empty(void *ctx)
{
    struct serial_in *s = ctx;
    kobject_signal(&s->base, SIG_READABLE, 0);
}

static void serial_in_zero(struct kobject *obj)
{
    struct serial_in *s = (struct serial_in *)obj;
    if (s->started) {
        s->started = false;
        serial_rx_stop();   /* no notify() calls after this */
    }
}

static void serial_in_destroy(struct kobject *obj)
{
    struct serial_in *s = (struct serial_in *)obj;
    serial_in_zero(obj);   /* never had a handle: stop here */
    uncharge(s->job);
    kfree(s);
}

static const struct kobject_ops serial_in_ops = {
    .name = "serial",
    .destroy = serial_in_destroy,
    .on_zero_handles = serial_in_zero,
};

status_t serial_in_create(struct job *job, struct kobject **out)
{
    if (!serial_present())
        return ERR_NOT_FOUND;
    struct serial_in *s = kzalloc(sizeof(*s));
    if (!s)
        return ERR_NO_MEMORY;
    status_t st = charge(job);
    if (st != OK) {
        kfree(s);
        return st;
    }
    s->job = job;
    kobject_init(&s->base, OBJ_SERIAL, &serial_in_ops, "serial", 0);
    st = serial_rx_start(serial_in_notify, s);
    if (st != OK) {
        kobject_unref(&s->base);
        return st;
    }
    s->started = true;
    *out = &s->base;
    return OK;
}

size_t serial_in_read(struct kobject *in, char *buf, size_t cap)
{
    return serial_rx_read(buf, cap, serial_in_empty, in);
}

/* ---- the screen ------------------------------------------------------------- */

struct screen {
    struct kobject base;   /* OBJ_SCREEN */
    struct job    *job;    /* charged one handle unit (a reference) */
    bool           owns;   /* fbcon_take succeeded: release on zero handles */
};

static void screen_zero(struct kobject *obj)
{
    struct screen *s = (struct screen *)obj;
    if (s->owns) {
        s->owns = false;
        fbcon_release();
        kprintf("screen: back to the kernel's log\n");
    }
}

static void screen_destroy(struct kobject *obj)
{
    struct screen *s = (struct screen *)obj;
    screen_zero(obj);
    uncharge(s->job);
    kfree(s);
}

static const struct kobject_ops screen_ops = {
    .name = "screen",
    .destroy = screen_destroy,
    .on_zero_handles = screen_zero,
};

status_t screen_take(struct job *job, struct fb_info *info, struct vmo **vmo_out,
                     struct kobject **owner)
{
    struct boot_framebuffer fb;
    if (!fbcon_geometry(&fb) || (fb.phys & (PAGE_SIZE - 1)))
        return ERR_NOT_FOUND;
    uint64_t size = ALIGN_UP((uint64_t)fb.pitch * fb.height, PAGE_SIZE);
    if (pmm_range_has_ram(fb.phys, size))
        return ERR_ACCESS_DENIED;   /* never hand out RAM, whatever the loader says */

    struct screen *s = kzalloc(sizeof(*s));
    if (!s)
        return ERR_NO_MEMORY;
    status_t st = charge(job);
    if (st != OK) {
        kfree(s);
        return st;
    }
    s->job = job;
    kobject_init(&s->base, OBJ_SCREEN, &screen_ops, "screen", 0);

    struct vmo *v;
    st = vmo_create_physical(fb.phys, size, VM_WC, &v);
    if (st == OK && (st = vmo_set_job(v, job)) != OK)
        kobject_unref(vmo_kobject(v));
    if (st == OK && (st = fbcon_take()) != OK)
        kobject_unref(vmo_kobject(v));
    if (st != OK) {
        kobject_unref(&s->base);
        return st;
    }
    s->owns = true;
    *info = (struct fb_info){
        .width = fb.width, .height = fb.height, .pitch = fb.pitch, .bpp = fb.bpp,
        .red_shift = fb.red_shift, .green_shift = fb.green_shift,
        .blue_shift = fb.blue_shift, .size = size,
    };
    *vmo_out = v;
    *owner = &s->base;
    kprintf("screen: taken by %s (%ux%u)\n",
            process_current() ? process_name(process_current()) : "the kernel", fb.width,
            fb.height);
    return OK;
}

void screen_owner_drop(struct kobject *owner)
{
    struct khandle kh = khandle_from_new(owner, SCREEN_RIGHTS);
    kobject_ref(owner);
    khandle_release(&kh);   /* handles 1 -> 0: gives the screen back */
    kobject_unref(owner);
}

/* ---- the system calls -------------------------------------------------------- */

int64_t sysc_klog_open(handle_t root, uint64_t out)
{
    SYSC_TABLE(t);
    status_t st = sysinfo_check_root(t, root, RIGHT_ROOT_KLOG);
    if (st != OK)
        return st;
    struct kobject *r;
    st = klog_reader_create(t->job, &r);
    return st == OK ? sysc_publish(t, r, READER_RIGHTS, out) : st;
}

static status_t to_user(void *ctx, uint64_t off, const char *text, size_t n)
{
    return copy_to_user(*(uint64_t *)ctx + off, text, n) == OK ? OK : ERR_INVALID_ARGS;
}

int64_t sysc_klog_read(handle_t reader, uint64_t pos, uint64_t buf, uint64_t cap, uint64_t first)
{
    SYSC_TABLE(t);
    struct kobject *r;
    status_t st = handle_get(t, reader, OBJ_KLOG, RIGHT_READ, &r, NULL);
    if (st != OK)
        return st;
    if (cap > KLOG_READ_MAX)
        cap = KLOG_READ_MAX;
    uint64_t start = 0, done = 0;
    st = klog_reader_read_to(r, pos, cap, to_user, &buf, &start, &done);
    kobject_unref(r);
    if (st == OK && copy_to_user(first, &start, sizeof(start)) != OK)
        st = ERR_INVALID_ARGS;
    return st == OK ? (int64_t)done : st;
}

int64_t sysc_klog_name(handle_t reader, uint64_t uname, uint64_t len)
{
    SYSC_TABLE(t);
    struct kobject *r;
    status_t st = handle_get(t, reader, OBJ_KLOG, RIGHT_READ, &r, NULL);
    if (st != OK)
        return st;
    kobject_unref(r);   /* only the right was needed */
    char name[KLOG_NAME_MAX];
    if (len == 0 || len >= sizeof(name) || copy_from_user(name, uname, len) != OK)
        return ERR_INVALID_ARGS;
    return kexec_set_log_name(name, len);
}

int64_t sysc_framebuffer_take(handle_t root, uint64_t uinfo, uint64_t uvmo, uint64_t uowner)
{
    SYSC_TABLE(t);
    status_t st = sysinfo_check_root(t, root, RIGHT_WRITE);
    if (st != OK)
        return st;
    struct fb_info info;
    struct vmo *v;
    struct kobject *owner;
    st = screen_take(t->job, &info, &v, &owner);
    if (st != OK)
        return st;
    /* The owner first: if anything after it fails, closing it gives the
     * screen back. */
    handle_t ho;
    struct khandle kh = khandle_from_new(owner, SCREEN_RIGHTS);
    st = handle_insert(t, &kh, &ho);
    if (st != OK) {
        khandle_release(&kh);   /* -> screen_zero: released */
        kobject_unref(vmo_kobject(v));
        return st;
    }
    if (copy_to_user(uinfo, &info, sizeof(info)) != OK ||
        copy_to_user(uowner, &ho, sizeof(ho)) != OK) {
        handle_close(t, ho);
        kobject_unref(vmo_kobject(v));
        return ERR_INVALID_ARGS;
    }
    st = sysc_publish(t, vmo_kobject(v), FB_VMO_RIGHTS, uvmo);
    if (st != OK)
        handle_close(t, ho);
    return st;
}

int64_t sysc_debug_command(handle_t root, uint64_t ucmd, uint64_t len)
{
    SYSC_TABLE(t);
    status_t st = sysinfo_check_root(t, root, RIGHT_ROOT_DEBUG);
    if (st != OK)
        return st;
    char cmd[64];
    if (len == 0 || len >= sizeof(cmd))
        return ERR_INVALID_ARGS;
    if (copy_from_user(cmd, ucmd, len) != OK)
        return ERR_INVALID_ARGS;
    cmd[len] = '\0';
    struct job *scope = job_root_of(t->job);
    int64_t r = dbgcmd_run_from(cmd, len, scope, t->job);
    job_unref(scope);
    return r;
}

int64_t sysc_reboot(handle_t root)
{
    SYSC_TABLE(t);
    status_t st = sysinfo_check_root(t, root, RIGHT_ROOT_REBOOT);
    if (st != OK)
        return st;
    kprintf("reboot: asked by %s\n", process_name(process_current()));
    machine_reboot();
}

int64_t sysc_serial_open(handle_t root, uint64_t out)
{
    SYSC_TABLE(t);
    status_t st = sysinfo_check_root(t, root, RIGHT_ROOT_SERIAL);
    if (st != OK)
        return st;
    struct kobject *s;
    st = serial_in_create(t->job, &s);
    return st == OK ? sysc_publish(t, s, SERIAL_RIGHTS, out) : st;
}

int64_t sysc_serial_read(handle_t h, uint64_t buf, uint64_t cap)
{
    SYSC_TABLE(t);
    struct kobject *s;
    status_t st = handle_get(t, h, OBJ_SERIAL, RIGHT_READ, &s, NULL);
    if (st != OK)
        return st;
    if (cap > SERIAL_IO_MAX)
        cap = SERIAL_IO_MAX;
    char chunk[CHUNK];
    uint64_t done = 0;
    while (done < cap) {
        size_t want = cap - done < CHUNK ? cap - done : CHUNK;
        size_t n = serial_in_read(s, chunk, want);
        if (n && copy_to_user(buf + done, chunk, n) != OK) {
            st = ERR_INVALID_ARGS;   /* those bytes are lost: the caller's fault */
            break;
        }
        done += n;
        if (n < want)
            break;
    }
    kobject_unref(s);
    return st == OK ? (int64_t)done : st;
}

int64_t sysc_serial_write(handle_t root, uint64_t buf, uint64_t len)
{
    SYSC_TABLE(t);
    status_t st = sysinfo_check_root(t, root, RIGHT_WRITE);
    if (st != OK)
        return st;
    if (len > SERIAL_IO_MAX)
        return ERR_OUT_OF_RANGE;
    char chunk[CHUNK];
    while (len) {
        uint64_t n = len < CHUNK ? len : CHUNK;
        if (copy_from_user(chunk, buf, n) != OK)
            return ERR_INVALID_ARGS;
        serial_write(chunk, n);
        buf += n;
        len -= n;
    }
    return OK;
}
