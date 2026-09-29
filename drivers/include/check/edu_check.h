/* The edu client check (M6 phase 2): what a client of the edu driver
 * verifies, the same code in both modes. kernel/drivers/kdevmgr.c runs it
 * in a kernel process against the edu driver as a kernel process
 * (`drivers=kernel`, ktest driver_kernel_edu); utest runs it against the
 * edu driver process devmgr started. Only <jam/driver.h> and the generated
 * <idl/edu.h> client: it works wherever drv_* do (a driver in either build,
 * or any program linked with libos). Failures are logged with drv_log. */
#pragma once

#include <jam/driver.h>
#include <idl/edu.h>

struct edu_check_result {
    uint32_t fact10;     /* factorial(10) as the device computed it */
    uint64_t dma_ns;     /* one 4 KiB RAM -> device -> RAM round trip */
    uint64_t msi_ns;     /* raise -> driver thread awake, median of EDU_CHECK_IRQS */
    uint32_t checks;     /* how many checks ran */
};

#define EDU_CHECK_IRQS 9

static inline uint32_t edu_check_fact(uint32_t n)
{
    uint32_t r = 1;
    for (uint32_t i = 2; i <= n; i++)
        r *= i;
    return r;
}

/* OK if every check passed (the first failure's status otherwise). Every
 * call has `deadline_ns`. */
static inline status_t edu_check(handle_t ch, uint64_t deadline_ns, struct edu_check_result *r)
{
    static const uint32_t ns[] = { 10, 0, 1, 5, 12, 13, 20 };
    static const uint32_t lens[] = { 4096, 1, 100, 4095 };
    status_t st;
    *r = (struct edu_check_result){ 0 };
    for (unsigned i = 0; i < sizeof(ns) / sizeof(ns[0]); i++) {
        uint32_t f = 0;
        r->checks++;
        st = edu_factorial_until(ch, deadline_ns, ns[i], &f);
        if (st != OK || f != edu_check_fact(ns[i])) {
            drv_log("edu check: factorial(%u) = %u (%s), want %u", ns[i], f, status_str(st),
                    edu_check_fact(ns[i]));
            return st != OK ? st : ERR_INTERNAL;
        }
        if (i == 0)
            r->fact10 = f;
    }
    for (unsigned i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
        uint64_t t0 = drv_clock_ns();
        r->checks++;
        st = edu_dma_roundtrip_until(ch, deadline_ns, lens[i]);
        if (st != OK) {
            drv_log("edu check: DMA round trip of %u bytes: %s", lens[i], status_str(st));
            return st;
        }
        if (i == 0)
            r->dma_ns = drv_clock_ns() - t0;
    }
    /* What the driver must refuse (the device would abort QEMU on a DMA
     * outside its 4 KiB buffer). */
    r->checks += 2;
    if ((st = edu_dma_roundtrip_until(ch, deadline_ns, 0)) != ERR_INVALID_ARGS ||
        (st = edu_dma_roundtrip_until(ch, deadline_ns, 4097)) != ERR_INVALID_ARGS) {
        drv_log("edu check: a DMA of 0 or 4097 bytes gave %s, want ERR_INVALID_ARGS",
                status_str(st));
        return ERR_INTERNAL;
    }
    uint64_t lat[EDU_CHECK_IRQS];
    for (unsigned i = 0; i < EDU_CHECK_IRQS; i++) {
        r->checks++;
        st = edu_raise_irq_until(ch, deadline_ns, &lat[i]);
        if (st != OK) {
            drv_log("edu check: raise_irq: %s", status_str(st));
            return st;
        }
        for (unsigned j = i; j > 0 && lat[j - 1] > lat[j]; j--) {
            uint64_t x = lat[j];
            lat[j] = lat[j - 1];
            lat[j - 1] = x;
        }
    }
    r->msi_ns = lat[EDU_CHECK_IRQS / 2];
    /* A held DMA (dma_start) is finished by the next call. */
    uint64_t addr = 0;
    uint32_t f = 0;
    r->checks++;
    if ((st = edu_dma_start_until(ch, deadline_ns, 4096, &addr)) != OK ||
        (st = edu_factorial_until(ch, deadline_ns, 10, &f)) != OK || f != 3628800u ||
        addr == 0 || addr >= (1ull << 32)) {
        drv_log("edu check: dma_start then factorial: %s, addr %#lx, %u", status_str(st),
                (unsigned long)addr, f);
        return st != OK ? st : ERR_INTERNAL;
    }
    return OK;
}
