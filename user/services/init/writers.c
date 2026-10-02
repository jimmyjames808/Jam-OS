/* init: the table of log writers the console trusts (<logwriters.h>):
 * init's koid, and devmgr's and logd's as each (re)starts. The page is
 * mapped here writable and handed to each console read-only; each field is
 * one atomic store, which the console's atomic loads see whole. */
#include <logwriters.h>
#include "init.h"

static handle_t page;                 /* the VMO (0: none: the console trusts no process) */
static struct log_writers *table;     /* our mapping of it */

void writers_init(void)
{
    uint64_t addr = 0;
    struct process_info info;
    status_t st = jam_vmo_create(PAGE_SIZE, 0, HANDLE_INVALID, &page);
    if (st == OK)
        st = jam_vmar_map(startup_handle(SR_SELF_VMAR), page, 0, PAGE_SIZE,
                          VMAR_READ | VMAR_WRITE, &addr);
    if (st == OK)
        st = jam_process_get_info(startup_handle(SR_SELF_PROCESS), &info);
    if (st != OK) {
        printf("init: no log writers' table (%s): the console's notices stay quiet\n",
               status_str(st));
        if (page)
            jam_handle_close(page);
        page = HANDLE_INVALID;
        return;
    }
    table = (struct log_writers *)(uintptr_t)addr;
    __atomic_store_n(&table->init, info.koid, __ATOMIC_RELEASE);
}

void writers_started(unsigned i, handle_t proc)
{
    struct process_info info;
    if (!table || (i != DEVMGR && i != LOGD) || jam_process_get_info(proc, &info) != OK)
        return;
    __atomic_store_n(i == DEVMGR ? &table->devmgr : &table->logd, info.koid, __ATOMIC_RELEASE);
}

handle_t writers_for_console(void)
{
    /* (RIGHT_TRANSFER: spawn moves it into the startup message.) */
    handle_t h;
    return page && jam_handle_duplicate(page, RIGHT_READ | RIGHT_MAP | RIGHT_TRANSFER, &h) == OK
               ? h
               : HANDLE_INVALID;
}
