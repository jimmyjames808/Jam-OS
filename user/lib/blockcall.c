/* A filesystem service's block calls, tried again while they time out
 * (<blockcall.h>, which has the rules). */
#include <blockcall.h>
#include <idl/block.h>

static const char *const op_names[] = { "read", "write", "flush" };

const char *blockcall_name(enum blockcall_op op)
{
    return op_names[op];
}

static status_t call_once(const struct blockcall *b, enum blockcall_op op, uint64_t deadline,
                          uint64_t lba, uint32_t count)
{
    if (op == BLOCKCALL_READ)
        return block_read_until(b->ch, deadline, lba, count, 0);
    if (op == BLOCKCALL_WRITE)
        return block_write_until(b->ch, deadline, lba, count, 0);
    return block_sync_until(b->ch, deadline);
}

/* A read or a write got no answer: the driver may still move its blocks
 * through the buffer, so nothing here may touch it again. */
static _Noreturn void lost(const struct blockcall *b, enum blockcall_op op, uint64_t lba,
                           uint32_t count, const struct blockcall_tries *t)
{
    char buf[48];
    printf("%s %s: %s of %u blocks at %lu got no answer%s: the disk may still move its "
           "data through the buffer, so it is left alone; ending, for an instance on a new "
           "channel\n", b->svc, b->name, op_names[op], count, (unsigned long)lba,
           blockcall_tried(t, buf, sizeof(buf)));
    jam_process_exit(1);
}

status_t blockcall(const struct blockcall *b, enum blockcall_op op, uint64_t lba, uint32_t count,
                   struct blockcall_tries *t)
{
    uint64_t wait = b->quick_wait ? BLOCKCALL_QUICK
                    : b->in_order ? BLOCKCALL_WAIT
                                  : BLOCKCALL_WAIT_DEEP;
    uint64_t start = now(), end = start + (b->quick_wait ? 2 * BLOCKCALL_QUICK : BLOCKCALL_BUDGET);
    uint64_t pause = b->quick_pause ? NS_PER_MS : BLOCKCALL_PAUSE;
    bool again = op == BLOCKCALL_SYNC || b->in_order;
    bool unanswered;
    status_t st;
    t->n = 0;
    for (;;) {
        uint64_t deadline = now() + wait;
        if (again && deadline > end)
            deadline = end;   /* the tries share the budget */
        st = call_once(b, op, deadline, lba, count);
        t->n++;
        /* The kernel's ERR_TIMED_OUT, not the driver's answer (one just
         * at the deadline counts as none: the safe side). */
        unanswered = st == ERR_TIMED_OUT && now() >= deadline;
        if (st != ERR_TIMED_OUT || !again || t->n >= BLOCKCALL_TRIES || now() + pause >= end)
            break;
        jam_sleep_until(now() + pause);
        pause *= 2;
    }
    t->ns = now() - start;
    if (unanswered && op != BLOCKCALL_SYNC)
        lost(b, op, lba, count, t);
    if (st == OK && t->n > 1 && op == BLOCKCALL_SYNC)
        printf("%s %s: a flush of the disk's cache timed out; try %u went through, %lu ms "
               "after the first began\n", b->svc, b->name, t->n,
               (unsigned long)(t->ns / NS_PER_MS));
    else if (st == OK && t->n > 1)
        printf("%s %s: %s of %u blocks at %lu timed out; try %u went through, %lu ms after "
               "the first began\n", b->svc, b->name, op_names[op], count, (unsigned long)lba,
               t->n, (unsigned long)(t->ns / NS_PER_MS));
    return st;
}

const char *blockcall_tried(const struct blockcall_tries *t, char *buf, size_t n)
{
    if (t->n < 2)
        return "";
    snprintf(buf, n, " (%u tries in %lu ms)", t->n, (unsigned long)(t->ns / NS_PER_MS));
    return buf;
}
