/* Must be REJECTED: an entry for the kernel's .ktests table (or
 * __ex_table, .limine_requests, ...): a table only the kernel's linker
 * script reads has no business in a driver object. */
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
