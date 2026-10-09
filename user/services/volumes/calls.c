/* volumes: the loop's cooperative tasks (<jam/task.h>, user/lib/task.c)
 * and the calls they make.
 *
 * volumes' loop never waits inside a request (CODING-GUIDE "A loop that
 * serves never blocks"). An operation of several steps that wait on
 * another process (a remount: fsctl.stop, the service's end, a new
 * `block` channel from the disk's driver, the start; the ESP made
 * writable; a disk released; a sync of every mount; the stop; and every
 * start of a filesystem service, which needs a `block` channel) runs as a
 * task: straight-line code that sends each request without waiting
 * (<proto>_<method>_send) and then waits for its answer in call_wait,
 * which gives the CPU back to the loop meanwhile. The loop goes on
 * serving its channels and its port; it is the only reader of every
 * channel volumes holds, and hands each answer a task waits for to that
 * task (call_offer: by channel and transaction id); an answer nobody waits
 * for any more (it came too late) is dropped by the reader. A `later`
 * request (remount, release_disk, sync, stop, volesp's two) is answered by
 * its task when it is done.
 *
 * The rules a task keeps (all of this runs on the loop's one thread):
 * anything may have changed across a wait, so a task working on a
 * filesystem service remembers its `serial` and gives up, touching
 * nothing, when it moved (the slot was retired: the disk went, or volumes
 * stops); the loop runs every waiting task again after each of its rounds
 * (task_kick), and each looks at its condition again. */
#include "disk.h"

#define MAX_TASKS 32   /* operations and starts under way at once */

struct task_set *tasks;
static struct call *waiting;   /* the calls tasks wait for */

static void overflow(void *arg)
{
    (void)arg;
    say(true, "volumes: a task ran over its stack: stopping");
}

bool calls_init(void)
{
    const struct task_opts o = { .max_tasks = MAX_TASKS, .overflow = overflow };
    tasks = task_set_create(&o);
    return tasks != NULL;
}

bool in_task(void)
{
    return tasks && task_current(tasks) != NULL;
}

uint32_t call_txid(void)
{
    return idl_txid_next(&txids);
}

void call_begin(struct call *c, handle_t ch)
{
    c->ch = ch;
    c->txid = call_txid();
    c->done = false;
    c->st = ERR_TIMED_OUT;
    c->m.nh = 0;
    c->next = waiting;
    waiting = c;
}

void call_end(struct call *c)
{
    for (struct call **p = &waiting; *p; p = &(*p)->next)
        if (*p == c) {
            *p = c->next;
            break;
        }
}

status_t call_wait(struct call *c, uint64_t deadline)
{
    if (!in_task()) {
        call_end(c);
        return ERR_BAD_STATE;
    }
    while (!c->done && now() < deadline)
        task_wait(tasks, deadline);
    call_end(c);
    return c->done ? c->st : ERR_TIMED_OUT;
}

bool call_offer(handle_t ch, const void *rep, struct idl_msg *m)
{
    for (struct call *c = waiting; c; c = c->next) {
        if (c->done || c->ch != ch || c->txid != m->txid)
            continue;
        if (m->n > sizeof(c->rep)) {
            idl_msg_drop(m);
            c->st = ERR_INTERNAL;   /* no answer of ours is that big */
        } else {
            memcpy(c->rep, rep, m->n);
            c->m = *m;   /* its handles are the task's now */
            m->nh = 0;
            c->st = OK;
        }
        c->done = true;
        task_kick(tasks);
        return true;
    }
    return false;
}

void call_fail(handle_t ch)
{
    if (!ch)
        return;
    for (struct call *c = waiting; c; c = c->next)
        if (!c->done && c->ch == ch) {
            c->done = true;
            c->st = ERR_PEER_CLOSED;
            task_kick(tasks);
        }
}
