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

status_t blockcall(const struct blockcall *b, enum blockcall_op op, uint64_t lba, uint32_t count,
                   struct blockcall_tries *t)
{
    uint64_t start = now(), end = start + BLOCKCALL_BUDGET;
    uint64_t pause = b->quick_pause ? NS_PER_MS : BLOCKCALL_PAUSE;
    bool again = op == BLOCKCALL_SYNC || b->in_order;
    status_t st;
    t->n = 0;
    for (;;) {
        uint64_t at = now();
        st = call_once(b, op, at + BLOCKCALL_WAIT < end ? at + BLOCKCALL_WAIT : end, lba, count);
        t->n++;
        if (st != ERR_TIMED_OUT || !again || t->n >= BLOCKCALL_TRIES || now() + pause >= end)
            break;
        jam_sleep_until(now() + pause);
        pause *= 2;
    }
    t->ns = now() - start;
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
