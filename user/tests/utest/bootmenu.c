/* utest: the boot menu's check (<bootmenu.h>), which init asks of a menu
 * before `update -w` writes it to the stick: a menu shaped like
 * boot/limine.conf passes; each rule broken is refused for its own reason
 * at its own line; a file that isn't there is named; the files are asked
 * about once each, and only for a menu that passed the rest; random
 * damage never passes as anything it isn't. `make check` runs the same
 * file (tools/menucheck.c) over boot/limine.conf itself. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <bootmenu.h>
#include <check.h>
#include <os.h>
#include "utest.h"

/* boot/limine.conf's shape: globals, comments, two top entries (the
 * default and the previous build) and a directory with one entry. */
static const char good[] =
    "timeout: 3\n"                                     /* 1 */
    "\n"                                               /* 2 */
    "# the everyday boot\n"                            /* 3 */
    "/Jam OS\n"                                        /* 4 */
    "    protocol: limine\n"                           /* 5 */
    "    path: boot():/boot/jamos.elf\n"               /* 6 */
    "    module_path: boot():/boot/bootfs.img\n"       /* 7 */
    "    module_path: boot():/boot/jamos.elf\n"        /* 8 */
    "\n"                                               /* 9 */
    "/Jam OS (previous build)\n"                       /* 10 */
    "    protocol: limine\n"                           /* 11 */
    "    path: boot():/boot/prev-jamos.elf\n"          /* 12 */
    "    module_path: boot():/boot/prev-bootfs.img\n"  /* 13 */
    "    module_path: boot():/boot/prev-jamos.elf\n"   /* 14 */
    "\n"                                               /* 15 */
    "/Tests\n"                                         /* 16 */
    "//All tests\n"                                    /* 17 */
    "    protocol: limine\n"                           /* 18 */
    "    path: boot():/boot/jamos.elf\n"               /* 19 */
    "    module_path: boot():/boot/bootfs.img\n"       /* 20 */
    "    cmdline: ktest\n";                            /* 21 */

/* The stick's files: all four, unless `missing` names one. */
struct stick {
    const char *missing;
    unsigned    asked;    /* exists() calls */
};

static bool on_stick(void *ctx, const char *path)
{
    struct stick *s = ctx;
    s->asked++;
    return !s->missing || strcmp(path, s->missing);
}

/* good with the first `from` replaced by `to`, checked. */
static bool edited_check(const char *from, const char *to, struct stick *s,
                         struct bootmenu_result *r)
{
    static char text[sizeof(good) + 512];
    const char *at = strstr(good, from);
    size_t pre = at ? (size_t)(at - good) : 0, fl = strlen(from), tl = strlen(to);
    if (!at || sizeof(good) + tl > sizeof(text)) {
        r->why = BOOTMENU_WHYS;   /* the test's own mistake */
        return false;
    }
    memcpy(text, good, pre);
    memcpy(text + pre, to, tl);
    memcpy(text + pre + tl, at + fl, strlen(at + fl) + 1);
    return bootmenu_check(text, strlen(text), on_stick, s, r);
}

bool t_bootmenu_good(void)
{
    struct stick s = { 0 };
    struct bootmenu_result r;
    CHECK(bootmenu_check(good, strlen(good), on_stick, &s, &r));
    CHECK_EQ(r.why, BOOTMENU_OK);
    CHECK_EQ(r.entries, 3);
    CHECK_EQ(s.asked, 4);   /* four files, each asked about once */
    static const struct { const char *from, *to; } fine[] = {
        { "timeout: 3", "timeout: no" },
        { "timeout: 3", "timeout: 600" },
        { "/Tests", "/+Tests" },
        { "    path: boot():/boot/jamos.elf\n    module_path: boot():/boot/bootfs.img\n    cmdline",
          "    kernel_path: boot():/boot/jamos.elf\n    module_path: boot():/boot/bootfs.img\n"
          "    kernel_cmdline" },
        { "    cmdline: ktest", "    cmdline: ktest\n    comment: every kernel test" },
        { "\n# the everyday boot", "\n#\n   \n" },
    };
    for (unsigned i = 0; i < sizeof(fine) / sizeof(fine[0]); i++) {
        s.asked = 0;
        if (!edited_check(fine[i].from, fine[i].to, &s, &r))
            FAIL("allowed change %u refused: line %u: %s", i, r.line, bootmenu_why_str(r.why));
    }
    return true;
}

bool t_bootmenu_refusals(void)
{
    static const struct {
        const char *from, *to;
        uint32_t    why, line;
    } cases[] = {
        { "timeout: 3", "timeout: 0", BOOTMENU_BAD_TIMEOUT, 1 },
        { "timeout: 3", "timeout: 601", BOOTMENU_BAD_TIMEOUT, 1 },
        { "timeout: 3", "timeout: 03", BOOTMENU_BAD_TIMEOUT, 1 },
        { "timeout: 3\n", "", BOOTMENU_NO_TIMEOUT, 0 },
        { "timeout: 3\n", "timeout: 3\ntimeout: 4\n", BOOTMENU_GLOBAL_TWICE, 2 },
        { "timeout: 3\n", "timeout: 3\ndefault_entry: 2\n", BOOTMENU_UNKNOWN_GLOBAL, 2 },
        { "timeout: 3\n", "timeout: 3\nserial: yes\n", BOOTMENU_UNKNOWN_GLOBAL, 2 },
        { "timeout: 3\n", "timeout: 3\nTIMEOUT: 3\n", BOOTMENU_UNKNOWN_GLOBAL, 2 },
        { "timeout: 3\n", "timeout: 3\n${K}=x\n", BOOTMENU_BAD_BYTE, 2 },
        { "timeout: 3\n", "timeout: 3\r\n", BOOTMENU_BAD_BYTE, 1 },
        { "# the everyday", "#\tthe everyday", BOOTMENU_BAD_BYTE, 3 },
        { "# the everyday", "  # the everyday", BOOTMENU_OPTION_OUTSIDE, 3 },
        { "    cmdline: ktest\n", "    cmdline: ktest\nverbose: yes\n",
          BOOTMENU_GLOBAL_IN_ENTRY, 22 },
        { "    protocol: limine", "    protocol: linux", BOOTMENU_BAD_PROTOCOL, 5 },
        { "    protocol: limine\n    path", "    path", BOOTMENU_NO_PROTOCOL, 4 },
        { "    protocol: limine", "    protocol: limine\n    protocol: limine",
          BOOTMENU_OPTION_TWICE, 6 },
        { "    path: boot():/boot/jamos.elf",
          "    path: boot():/boot/jamos.elf\n    kernel_path: boot():/boot/jamos.elf",
          BOOTMENU_OPTION_TWICE, 7 },
        { "    cmdline: ktest", "    cmdline: ktest\n    cmdline: bench",
          BOOTMENU_OPTION_TWICE, 22 },
        { "    cmdline: ktest", "    resolution: 800x600", BOOTMENU_UNKNOWN_OPTION, 21 },
        { "    cmdline: ktest", "    cmdline:ktest", BOOTMENU_UNKNOWN_OPTION, 21 },
        { "    cmdline: ktest", "    cmdline: ", BOOTMENU_UNKNOWN_OPTION, 21 },
        { "    path: boot():/boot/jamos.elf\n    module_path: boot():/boot/bootfs.img\n    "
          "module_path: boot():/boot/jamos.elf", "    module_path: boot():/boot/bootfs.img",
          BOOTMENU_NO_PATH, 4 },
        { "path: boot():/boot/jamos.elf", "path: boot():/boot/../jamos.elf",
          BOOTMENU_BAD_PATH, 6 },
        { "path: boot():/boot/jamos.elf", "path: boot():/boot//jamos.elf", BOOTMENU_BAD_PATH, 6 },
        { "path: boot():/boot/jamos.elf", "path: boot():/boot/JAMOS.ELF", BOOTMENU_BAD_PATH, 6 },
        { "path: boot():/boot/jamos.elf", "path: boot():boot/jamos.elf", BOOTMENU_BAD_PATH, 6 },
        { "path: boot():/boot/jamos.elf", "path: boot():/boot/", BOOTMENU_BAD_PATH, 6 },
        { "path: boot():/boot/jamos.elf", "path: boot():/boot/jamos.elf#ab12",
          BOOTMENU_BAD_PATH, 6 },
        { "path: boot():/boot/jamos.elf", "path: hdd(1:1):/boot/jamos.elf",
          BOOTMENU_BAD_PATH, 6 },
        { "path: boot():/boot/jamos.elf", "path: boot():/boot/memtest.efi",
          BOOTMENU_NOT_JAMOS, 4 },
        { "module_path: boot():/boot/bootfs.img", "module_path: boot():/boot/prev-bootfs.img",
          BOOTMENU_BAD_PAIR, 4 },
        { "    module_path: boot():/boot/bootfs.img\n    module_path: boot():/boot/jamos.elf\n",
          "", BOOTMENU_BAD_PAIR, 4 },
        { "/Jam OS\n    protocol: limine\n    path: boot():/boot/jamos.elf\n"
          "    module_path: boot():/boot/bootfs.img",
          "/Jam OS\n    protocol: limine\n    path: boot():/boot/prev-jamos.elf\n"
          "    module_path: boot():/boot/prev-bootfs.img", BOOTMENU_BAD_DEFAULT, 4 },
        { "timeout: 3\n", "timeout: 3\n/Dir\n//Child\n    protocol: limine\n"
          "    path: boot():/boot/jamos.elf\n    module_path: boot():/boot/bootfs.img\n",
          BOOTMENU_BAD_DEFAULT, 2 },
        { "/Jam OS (previous build)", "/Jam OS (old build)", BOOTMENU_NO_PREVIOUS, 0 },
        { "prev-jamos.elf\n    module_path: boot():/boot/prev-bootfs.img",
          "jamos.elf\n    module_path: boot():/boot/bootfs.img", BOOTMENU_BAD_PREVIOUS, 10 },
        { "timeout: 3\n", "timeout: 3\n//Orphan\n", BOOTMENU_PARENTLESS, 2 },
        { "/Tests\n//All tests", "//All tests", BOOTMENU_OPTION_IN_DIR, 16 },
        { "/Tests\n", "/Tests\n    comment: the tests\n", BOOTMENU_OPTION_IN_DIR, 18 },
        { "/Jam OS\n", "/+Jam OS\n", BOOTMENU_BAD_PLUS, 4 },
        { "//All tests", "//+All tests", BOOTMENU_BAD_PLUS, 17 },
        { "//All tests", "///All tests", BOOTMENU_BAD_ENTRY, 17 },
        { "/Tests", "/", BOOTMENU_BAD_ENTRY, 16 },
        { "    cmdline: ktest\n", "    cmdline: ktest", BOOTMENU_NO_NEWLINE, 21 },
        { "    module_path: boot():/boot/jamos.elf\n", "    module_path: boot():/boot/jamos.elf\n"
          "    module_path: boot():/boot/jamos.elf\n    module_path: boot():/boot/jamos.elf\n"
          "    module_path: boot():/boot/jamos.elf\n", BOOTMENU_TOO_MANY, 11 },
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct stick s = { 0 };
        struct bootmenu_result r;
        bool ok = edited_check(cases[i].from, cases[i].to, &s, &r);
        if (ok || r.why != cases[i].why || r.line != cases[i].line || s.asked)
            FAIL("case %u: %s at line %u (%u files asked), not %s at line %u", i,
                 ok ? "passed" : bootmenu_why_str(r.why), r.line, s.asked,
                 bootmenu_why_str(cases[i].why), cases[i].line);
    }
    return true;
}

bool t_bootmenu_files(void)
{
    struct stick s = { .missing = "/boot/prev-bootfs.img" };
    struct bootmenu_result r;
    CHECK(!bootmenu_check(good, strlen(good), on_stick, &s, &r));
    CHECK_EQ(r.why, BOOTMENU_NO_FILE);
    CHECK_EQ(r.line, 13);
    CHECK(!strcmp(r.path, "/boot/prev-bootfs.img"));
    CHECK_EQ(r.entries, 0);
    static char big[BOOTMENU_MAX + 2];
    memset(big, '#', sizeof(big));
    for (size_t i = 100; i < sizeof(big); i += 101)
        big[i] = '\n';
    big[sizeof(big) - 1] = '\n';
    s = (struct stick){ 0 };
    CHECK(!bootmenu_check(big, sizeof(big), on_stick, &s, &r) && r.why == BOOTMENU_TOO_BIG);
    CHECK(!bootmenu_check(good, 0, on_stick, &s, &r) && r.why == BOOTMENU_EMPTY);
    char line[300];
    memset(line, '#', sizeof(line));
    memcpy(line + 256, "\n", 2);
    CHECK(!bootmenu_check(line, 257, on_stick, &s, &r) && r.why == BOOTMENU_LONG_LINE);
    CHECK(bootmenu_check(line + 1, 256, on_stick, &s, &r) == false && r.why == BOOTMENU_NO_ENTRY);
    CHECK_EQ(s.asked, 0);
    return true;
}

/* Random damage to the good menu: never a crash, every verdict a real
 * one, and a menu that passes still has its previous-build entry, and
 * was asked about at most BOOTMENU_FILES files. */
bool t_bootmenu_fuzz(void)
{
    static char text[sizeof(good)];
    uint32_t x = 2026;
    for (unsigned round = 0; round < 4000; round++) {
        memcpy(text, good, sizeof(good));
        size_t len = sizeof(good) - 1;
        for (unsigned k = 0; k < 1 + round % 3; k++) {
            x = x * 1103515245u + 12345u;
            size_t at = (x >> 8) % len;
            x = x * 1103515245u + 12345u;
            static const char picks[] = " /:#.\nabjp-";   /* bytes the syntax turns on */
            text[at] = round & 1 ? (char)(x >> 24) : picks[(x >> 24) % 11];
        }
        struct stick s = { 0 };
        struct bootmenu_result r;
        bool ok = bootmenu_check(text, len, on_stick, &s, &r);
        if (r.why >= BOOTMENU_WHYS || ok != (r.why == BOOTMENU_OK) || r.line > 30)
            FAIL("round %u: verdict %u at line %u", round, r.why, r.line);
        if (ok && (!strstr(text, "\n/Jam OS (previous build)\n") || s.asked > BOOTMENU_FILES))
            FAIL("round %u passed without its rules", round);
    }
    return true;
}
