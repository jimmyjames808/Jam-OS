/* null: the M6 test driver. It serves the `null` protocol
 * (abi/idl/null.idl: ping, add, reverse, make_vmo) on its DR_SERVE channel until the
 * client closes it, then exits 0. Built both ways like every driver: into
 * the kernel (a kernel process) and as drv/null in bootfs (a process). */
#include <jam/driver.h>
#include <idl/null.h>

struct null_state {
    uint64_t calls;
};

static status_t do_ping(void *ctx, uint64_t value, uint64_t *out_value)
{
    ((struct null_state *)ctx)->calls++;
    *out_value = value;
    return OK;
}

static status_t do_add(void *ctx, uint32_t a, uint32_t b, uint32_t *out_sum)
{
    ((struct null_state *)ctx)->calls++;
    *out_sum = a + b;
    return OK;
}

static status_t do_reverse(void *ctx, const uint8_t data[16], uint8_t out_data[16])
{
    ((struct null_state *)ctx)->calls++;
    for (int i = 0; i < 16; i++)
        out_data[i] = data[15 - i];
    return OK;
}

static status_t do_make_vmo(void *ctx, uint32_t size, uint8_t fill, handle_t *out_vmo,
                            uint64_t *out_size_back)
{
    ((struct null_state *)ctx)->calls++;
    if (size == 0 || size > 65536)
        return ERR_INVALID_ARGS;
    handle_t v;
    status_t st = drv_vmo_create(size, 0, &v);
    if (st != OK)
        return st;
    if ((st = drv_vmo_write(v, 0, &fill, 1)) != OK) {
        drv_handle_close(v);
        return st;
    }
    *out_vmo = v;
    *out_size_back = size;
    return OK;
}

static const struct null_ops ops = {
    .ping = do_ping,
    .add = do_add,
    .reverse = do_reverse,
    .make_vmo = do_make_vmo,
};

int driver_main(const struct driver_start *s)
{
    handle_t ch = drv_handle(s, DR_SERVE);
    if (ch == HANDLE_INVALID) {
        drv_log("no DR_SERVE channel: nothing to serve");
        return 2;
    }
    struct null_state *st = drv_malloc(sizeof(*st));
    if (!st)
        return 3;
    st->calls = 0;
    status_t r = null_serve(ch, &ops, st);
    drv_log("client gone after %lu call(s) (%s)", (unsigned long)st->calls,
            r == OK ? "closed" : status_str(r));
    drv_free(st);
    return r == OK ? 0 : 1;
}
