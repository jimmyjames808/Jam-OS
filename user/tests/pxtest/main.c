/* pxtest: a program for the tests of programs run from /data from their
 * pages (tools/pager-test.sh, case exec; the shell's sh_dataprog.c). Its
 * test copies it to /data and allows it there.
 *
 * Beside its code it holds a table of TABLE_PAGES pages of constant data
 * in its read-only segment, which the loader maps straight from the
 * program's VMO: a page of it is fetched from /data only when it is
 * touched, and the test counts fat's fetches. The table starts with
 * TABLE_MARK, so a test can find it in the file and change one of its
 * pages; a writable variable with a value of its own (in the data
 * segment, which spawn copies) and the ELF header's page are the other
 * places a test changes.
 *   pxtest lazy             touch nothing of the table
 *   pxtest touch [cue]      touch every page of the table; with a cue (a
 *                           path), then wait for that file to appear
 *                           (polled, CUE_WAIT at most) and touch them all
 *                           again
 *   pxtest sleep <ms>       touch the first half of the table, sleep, then
 *                           the second half (the stick is pulled meanwhile)
 * It says what it did; a page the kernel can't give it (changed since it
 * was allowed, or its stick gone) ends it with the kernel's line instead.
 * Exit codes: 0 done; 1 the data segment's value was wrong; 3 the cue
 * never came; 64 usage. */
#include <os.h>
#include <wants.h>

/* /data: the cue file; deputy: utest starts it through /svc/exec, a caller
 * init can't vouch for (init's exec.c, "who is raised"). */
JAM_WANTS("mount /data r\n"
          "deputy\n");

#define TABLE_PAGES 128
#define TABLE_MARK  "PXTEST-TABLE-0123456789"
#define DATA_VALUE  0x50585444u   /* "DTXP": the data segment's word */
#define CUE_WAIT    (120 * NS_PER_S)
#define CUE_POLL    (50 * NS_PER_MS)

/* const and initialised: in .rodata, every byte in the file. */
static const uint8_t table[TABLE_PAGES * PAGE_SIZE] __attribute__((aligned(4096))) = TABLE_MARK;
static volatile uint32_t data_word = DATA_VALUE;   /* in .data */

/* Read one byte of each page of the table in [from, to): a load each, so
 * every page is faulted in. */
static unsigned touch(unsigned from, unsigned to)
{
    unsigned sum = 0;
    for (unsigned p = from; p < to; p++)
        sum += ((const volatile uint8_t *)table)[p * PAGE_SIZE + PAGE_SIZE / 2];
    return sum;
}

static bool wait_cue(const char *cue)
{
    uint64_t end = now() + CUE_WAIT;
    printf("pxtest: waiting for %s\n", cue);
    while (fs_stat(cue, NULL, NULL, NULL) != OK) {
        if (now() >= end)
            return false;
        jam_sleep_until(now() + CUE_POLL);
    }
    return true;
}

static int usage(void)
{
    printf("usage: pxtest lazy | touch [cue] | sleep <ms>\n");
    return 64;
}

int main(int argc, char **argv)
{
    if (data_word != DATA_VALUE) {
        printf("pxtest: the data segment holds %x, not %x\n", data_word, DATA_VALUE);
        return 1;
    }
    if (argc == 2 && !strcmp(argv[1], "lazy")) {
        printf("pxtest: lazy: none of the table touched\n");
        return 0;
    }
    if ((argc == 2 || argc == 3) && !strcmp(argv[1], "touch")) {
        unsigned sum = touch(0, TABLE_PAGES);
        printf("pxtest: touched %u pages (%u)\n", TABLE_PAGES, sum);
        if (argc == 3 && !wait_cue(argv[2]))
            return 3;
        if (argc == 3)
            printf("pxtest: touched %u pages again (%u)\n", TABLE_PAGES, touch(0, TABLE_PAGES));
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "sleep")) {
        uint64_t ms = 0;
        for (const char *s = argv[2]; *s >= '0' && *s <= '9' && ms < 1000000; s++)
            ms = ms * 10 + (uint64_t)(*s - '0');
        unsigned sum = touch(0, TABLE_PAGES / 2);
        printf("pxtest: half the table touched (%u), sleeping %lu ms\n", sum, (unsigned long)ms);
        jam_sleep_until(now() + ms * NS_PER_MS);
        printf("pxtest: awake: the other half (%u)\n", touch(TABLE_PAGES / 2, TABLE_PAGES));
        return 0;
    }
    return usage();
}
