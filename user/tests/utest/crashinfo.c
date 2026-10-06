/* utest: a saved panic's text read back (libos <crashinfo.h>): the code
 * the kernel's panic screen showed, the report's intro, the panic's lines
 * and its backtrace, from a report shaped as logd writes it and the kernel
 * prints it; and text that only looks like one (codes of the wrong shape,
 * a cut file, bytes outside printable ASCII). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <crashinfo.h>
#include "utest.h"

static const char report[] =
    "Jam OS crash log: the end of the kernel log of boot-0007, which panicked after 12.345 s "
    "(panic 1 in a row), saved by the boot after it, Tue 7 Oct 2026 10:00.\n"
    "The panic: page fault (vector 14, error 2) at crash_pf+0x7\n"
    "The panic starts at byte 67 of the log below.\n"
    "--------\n"
    "[   12.300000] dbgcmd: crash pf\n"
    "[   12.300100] crash pf: here goes\n"
    "[   12.300200] \n"
    "[   12.300300]   *** JAM OS KERNEL PANIC *** on cpu 1, thread \"dbgcmd\" (other CPUs "
    "halted: 3)\n"
    "[   12.300400]   page fault (vector 14, error 2) at crash_pf+0x7\n"
    "[   12.300500] backtrace:\n"
    "[   12.300600]   #0  ffffffff800c0857  crash_pf+0x7\n"
    "[   12.300700]   #1  ffffffff800c1d60  selftest_crash+0x70\n"
    "[   12.300800]        ... same frame 3 more times\n"
    "[   12.300900] \n"
    "[   12.301000]   code JAM-PF-0018: page fault; its digits are the low 16 bits of the "
    "faulting address (0000000000000018)\n"
    "[   12.301100]   build Jam OS 0.0.31, git 2079f35\n";

bool t_crashinfo_report(void)
{
    struct crash_report r;
    CHECK(crash_read_intro(report, sizeof(report) - 1, &r));
    CHECK(!strcmp(r.boot, "boot-0007, which panicked after 12.345 s (panic 1 in a row)"));
    CHECK(!strcmp(r.message, "page fault (vector 14, error 2) at crash_pf+0x7"));
    CHECK_EQ(r.panic_at, 67);
    const char *text = strstr(report, "--------\n") + 9;
    CHECK_EQ(r.text_at, (uint64_t)(text - report));
    CHECK(!strncmp(text + r.panic_at, "[   12.300200]", 14));   /* where the panic's lines begin */
    const char *panic = text + r.panic_at;
    size_t n = strlen(panic);
    crash_read_panic(panic, n, &r);
    CHECK(!strcmp(r.code, "JAM-PF-0018"));
    CHECK(!strcmp(r.what, "page fault"));
    CHECK(!strcmp(r.where, "cpu 1, thread \"dbgcmd\""));
    CHECK(!strcmp(r.build, "Jam OS 0.0.31, git 2079f35"));
    static const char *const frames[] = { "#0  ffffffff800c0857  crash_pf+0x7",
                                          "#1  ffffffff800c1d60  selftest_crash+0x70",
                                          "... same frame 3 more times" };
    char line[80];
    size_t at = 0;
    for (unsigned i = 0; i < 3; i++) {
        CHECK(crash_next_frame(panic, n, &at, line, sizeof(line)));
        CHECK(!strcmp(line, frames[i]));
    }
    CHECK(!crash_next_frame(panic, n, &at, line, sizeof(line)));   /* the blank line ends it */
    return true;
}

bool t_crashinfo_code(void)
{
    static const struct {
        const char *text, *code;
    } rows[] = {
        { "  code JAM-PF-7F3A: page fault", "JAM-PF-7F3A" },
        { "code JAM-OOM-0042: out", "JAM-OOM-0042" },
        { "code JAM-AS-12AB", "JAM-AS-12AB" },                     /* at the end */
        { "code JAM-P-1234 then code JAM-GP-BEEF", "JAM-GP-BEEF" },  /* the first good one */
        { "code JAM-PFXX-1234", "" },      /* kind too long */
        { "code JAM-PF-12345", "" },       /* too many digits */
        { "code JAM-PF-12g4", "" },        /* not hex */
        { "code JAM-pf-1234", "" },        /* small letters */
        { "code JAM-PF-123", "" },         /* cut short */
        { "JAM-PF-1234 without its word", "" },
        { "", "" },
    };
    char code[CRASH_CODE_MAX];
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        bool found = crash_code(rows[i].text, strlen(rows[i].text), code);
        CHECK_EQ(found, rows[i].code[0] != 0);
        CHECK(!strcmp(code, rows[i].code));
    }
    /* Only the bytes given are read: a code cut off by n is none. */
    CHECK(!crash_code("code JAM-PF-7F3A", 14, code) && !code[0]);
    return true;
}

bool t_crashinfo_hostile(void)
{
    struct crash_report r;
    CHECK(!crash_read_intro("not a crash log\n", 16, &r));
    /* Every cut of the report: never a read past it, and a cut before the
     * "--------" line is no report. */
    size_t dash = (size_t)(strstr(report, "--------\n") - report);
    for (size_t n = 0; n < sizeof(report) - 1; n++) {
        bool ok = crash_read_intro(report, n, &r);
        CHECK_EQ(ok, n >= dash + 9);
        crash_read_panic(report, n, &r);
        size_t at = 0;
        char line[16];
        while (crash_next_frame(report, n, &at, line, sizeof(line)))
            CHECK(strlen(line) < sizeof(line));
    }
    /* Bytes outside printable ASCII come out as '?'; fields are cut. */
    static const char odd[] = "  *** JAM OS KERNEL PANIC *** on cpu \x01\xff\n  build Jam OS "
                              "0123456789012345678901234567890123456789012345678901234567890123"
                              "456789\n";
    memset(&r, 0, sizeof(r));
    crash_read_panic(odd, sizeof(odd) - 1, &r);
    CHECK(!strcmp(r.where, "cpu ??"));
    CHECK_EQ(strlen(r.build), sizeof(r.build) - 1);
    return true;
}
