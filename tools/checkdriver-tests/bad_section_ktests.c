/* Must be REJECTED: an entry placed in the kernel's .ktests table (or
 * __ex_table, .limine_requests, ...) is KEEP()'d by kernel/linker.ld even
 * though objcopy localises the symbol, so the kernel would run or obey it
 * outside the driver's process. */
#include <stdint.h>
#include <jam/driver.h>

static void evil(void) {}
__attribute__((used, section(".ktests"))) static const struct {
    const char *n;
    void (*f)(void);
} entry = { "driver_injected", evil };

int driver_main(const struct driver_start *s)
{
    (void)s;
    return 0;
}
