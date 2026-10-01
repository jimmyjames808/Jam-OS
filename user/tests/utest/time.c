/* utest: libos's calendar and time zones (<wallclock.h>), the wall clock's
 * system calls, and the settings file (<settings.h>): its parser on odd
 * text, and writing it on a FAT volume (bin/fat on a RAM disk) by a new
 * name and a rename. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <settings.h>
#include <wallclock.h>
#include "fattest.h"
#include "utest.h"

static bool has_nul(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (!s[i])
            return true;
    return false;
}

/* The local time at UTC t in zone `name`, as "2026-10-04 03:00:00 AEDT". */
static bool local_is(const char *name, int64_t t, const char *want)
{
    struct tz tz;
    char buf[48];
    if (!tz_parse(name, &tz)) {
        printf("utest: %s: zone %s not understood\n", utest_cur, name);
        return false;
    }
    time_format_iso(t, &tz, buf, sizeof(buf));
    if (strcmp(buf, want)) {
        printf("utest: %s: %s at %ld is \"%s\", want \"%s\"\n", utest_cur, name, (long)t, buf,
               want);
        return false;
    }
    return true;
}

bool t_time_calendar(void)
{
    struct civil c;
    CHECK_EQ(civil_days(1970, 1, 1), 0);
    CHECK_EQ(civil_days(2000, 3, 1), 11017);
    CHECK_EQ(civil_days(1969, 12, 31), -1);
    civil_from_secs(1709208000, &c);   /* 2024-02-29 12:00, a Thursday */
    CHECK(c.year == 2024 && c.month == 2 && c.day == 29 && c.hour == 12 && c.wday == 4);
    civil_from_secs(-1, &c);
    CHECK(c.year == 1969 && c.month == 12 && c.day == 31 && c.second == 59);
    return true;
}

/* Each zone just before and at its switches (2026), both ways. */
bool t_time_zones_switch(void)
{
    CHECK(local_is("Australia/Sydney", 1791043199, "2026-10-04 01:59:59 AEST"));
    CHECK(local_is("Australia/Sydney", 1791043200, "2026-10-04 03:00:00 AEDT"));
    CHECK(local_is("Australia/Sydney", 1775318399, "2026-04-05 02:59:59 AEDT"));
    CHECK(local_is("Australia/Sydney", 1775318400, "2026-04-05 02:00:00 AEST"));
    CHECK(local_is("Australia/Hobart", 1791043200, "2026-10-04 03:00:00 AEDT"));
    CHECK(local_is("Australia/Brisbane", 1791043200, "2026-10-04 02:00:00 AEST"));
    CHECK(local_is("Australia/Adelaide", 1791044999, "2026-10-04 01:59:59 ACST"));
    CHECK(local_is("Australia/Adelaide", 1791045000, "2026-10-04 03:00:00 ACDT"));
    CHECK(local_is("Australia/Perth", 1768435200, "2026-01-15 08:00:00 AWST"));
    CHECK(local_is("Pacific/Auckland", 1790431199, "2026-09-27 01:59:59 NZST"));
    CHECK(local_is("Pacific/Auckland", 1790431200, "2026-09-27 03:00:00 NZDT"));
    CHECK(local_is("Pacific/Auckland", 1775311200, "2026-04-05 02:00:00 NZST"));
    CHECK(local_is("Europe/London", 1774745999, "2026-03-29 00:59:59 GMT"));
    CHECK(local_is("Europe/London", 1774746000, "2026-03-29 02:00:00 BST"));
    CHECK(local_is("Europe/London", 1792889999, "2026-10-25 01:59:59 BST"));
    CHECK(local_is("Europe/London", 1792890000, "2026-10-25 01:00:00 GMT"));
    CHECK(local_is("America/New_York", 1772953200, "2026-03-08 03:00:00 EDT"));
    CHECK(local_is("America/New_York", 1793512800, "2026-11-01 01:00:00 EST"));
    CHECK(local_is("America/Los_Angeles", 1768435200, "2026-01-14 16:00:00 PST"));
    CHECK(local_is("UTC", 0, "1970-01-01 00:00:00 UTC"));
    CHECK(local_is("UTC+05:30", 0, "1970-01-01 05:30:00 UTC+05:30"));
    CHECK(local_is("-5", 0, "1969-12-31 19:00:00 UTC-05:00"));
    CHECK(local_is("", 1768435200, "2026-01-15 11:00:00 AEDT"));   /* the default: Sydney */
    return true;
}

bool t_time_zones_local(void)
{
    struct tz syd, lon, bad;
    CHECK(tz_parse("Australia/Sydney", &syd) && tz_parse("Europe/London", &lon));
    /* An ordinary local time, either side of the year. */
    int64_t l = civil_days(2026, 1, 15) * 86400 + 11 * 3600;
    CHECK_EQ(tz_local_to_utc(&syd, l), 1768435200);
    l = civil_days(2026, 7, 1) * 86400 + 10 * 3600;
    CHECK_EQ(tz_local_to_utc(&syd, l), 1782864000);
    /* 02:30 on 2026-04-05 happens twice in Sydney: standard time. */
    l = civil_days(2026, 4, 5) * 86400 + 2 * 3600 + 1800;
    CHECK_EQ(tz_local_to_utc(&syd, l), l - 600 * 60);
    /* 02:30 on 2026-10-04 never happens there: taken as daylight time. */
    l = civil_days(2026, 10, 4) * 86400 + 2 * 3600 + 1800;
    CHECK_EQ(tz_local_to_utc(&syd, l), l - 660 * 60);
    l = civil_days(2026, 7, 1) * 86400 + 12 * 3600;   /* BST */
    CHECK_EQ(tz_local_to_utc(&lon, l), l - 3600);
    /* What isn't a zone. */
    CHECK(!tz_parse("Mars/Olympus_Mons", &bad));
    CHECK(!tz_parse("UTC+15", &bad));
    CHECK(!tz_parse("+5:60", &bad));
    CHECK(!tz_parse("+5:3", &bad));
    CHECK(!tz_parse("Australia/Sydney ", &bad));
    CHECK(!tz_parse("A_name_that_is_longer_than_a_zone_can_be", &bad));
    CHECK(tz_parse("Sydney", &bad) && bad.std_min == 600 && bad.dst_min == 660);
    char buf[64];
    time_format(1791043200, &syd, buf, sizeof(buf), true);
    CHECK(!strcmp(buf, "Sun 4 Oct 2026 03:00:00 AEDT (UTC+11:00)"));
    return true;
}

/* The wall clock's calls: wallclock_get needs no handle; wallclock_set
 * needs RIGHT_MANAGE on the root, which no starter gives utest, so only
 * the refusals are checked (init sets the clock at every boot, and the
 * shell's `date -z` in cmds.txt). */
bool t_time_wallclock_calls(void)
{
    struct wall_clock w, w2;
    CHECK_ST(jam_wallclock_get(&w), OK);
    CHECK(w.flags & (WALLCLOCK_SET | WALLCLOCK_RTC));
    CHECK(w.utc_ns / (int64_t)NS_PER_S > 946684800);   /* after 2000 */
    CHECK(w.reserved == 0 && has_nul(w.zone, sizeof(w.zone)));
    CHECK_ST(jam_wallclock_get(&w2), OK);
    CHECK(w2.utc_ns - w.utc_ns == (int64_t)(w2.uptime_ns - w.uptime_ns));
    CHECK_ST(jam_wallclock_get((struct wall_clock *)8), ERR_INVALID_ARGS);
    handle_t v, root = startup_handle(SR_RESOURCE), rd;
    CHECK_ST(jam_vmo_create(4096, 0, HANDLE_INVALID, &v), OK);
    CHECK_ST(jam_wallclock_set(v, &w), ERR_WRONG_TYPE);
    CHECK_ST(jam_wallclock_set(0x7fff0000u, &w), ERR_BAD_HANDLE);
    CHECK_ST(jam_handle_close(v), OK);
    if (root) {
        CHECK_ST(jam_handle_duplicate(root, RIGHTS_BASIC | RIGHT_READ, &rd), OK);
        CHECK_ST(jam_wallclock_set(rd, &w), ERR_ACCESS_DENIED);
        CHECK_ST(jam_handle_close(rd), OK);
    }
    struct tz zone;
    int64_t utc = 0;
    (void)clock_now(&utc, &zone);
    int64_t then = w2.utc_ns / (int64_t)NS_PER_S;
    CHECK(utc >= then && utc - then < 60);
    CHECK(zone.name[0]);
    return true;
}

/* ---- the settings file ------------------------------------------------------------ */

/* settings_lookup of key in text: want (NULL: not there). */
static bool looked(const char *text, const char *key, const char *want)
{
    char v[SETTINGS_VALUE_MAX];
    bool found = settings_lookup(text, strlen(text), key, v, sizeof(v));
    if (found != (want != NULL) || (want && strcmp(v, want))) {
        printf("utest: %s: %s in \"%s\": %s, want %s\n", utest_cur, key, text,
               found ? v : "(none)", want ? want : "(none)");
        return false;
    }
    return true;
}

/* settings_edit of text: want. */
static bool edited(const char *text, const char *key, const char *value, const char *want)
{
    char out[512];
    size_t n = 0;
    status_t st = settings_edit(text, strlen(text), key, value, out, sizeof(out), &n);
    if (st != OK || n != strlen(want) || memcmp(out, want, n)) {
        printf("utest: %s: setting %s in \"%s\": %s \"%.*s\"\n", utest_cur, key, text,
               status_str(st), (int)n, out);
        return false;
    }
    return true;
}

bool t_settings_parse(void)
{
    CHECK(looked("", "volume", NULL));
    CHECK(looked("volume = -10", "volume", "-10"));            /* no newline at the end */
    CHECK(looked("volume=-10\n", "volume", "-10"));
    CHECK(looked("  volume\t=\t -10 \t\n", "volume", "-10"));
    CHECK(looked("volume = -10\r\n", "volume", "-10"));         /* written on Windows */
    CHECK(looked("# volume = -10\n", "volume", NULL));        /* a comment */
    CHECK(looked("  # volume = -10\n", "volume", NULL));
    CHECK(looked("music.folder = /usb0/My #1 Songs\n", "music.folder", "/usb0/My #1 Songs"));
    CHECK(looked("music.folder = /data/JA\xc5\xb8-Z\n", "music.folder", "/data/JA\xc5\xb8-Z"));
    CHECK(looked("volume = -10\nvolume = -20\n", "volume", "-20"));   /* the last counts */
    CHECK(looked("volume = -10\nvolume = \x1b[2J\n", "volume", "-10"));   /* not a value */
    CHECK(looked("volume = a\tb\n", "volume", NULL));
    CHECK(looked("volume\n", "volume", NULL));                 /* no '=' */
    CHECK(looked("= -10\n", "volume", NULL));
    CHECK(looked("vol ume = -10\n", "vol ume", NULL));
    CHECK(looked("volume =\n", "volume", ""));                /* empty is a value */
    CHECK(looked("a = 1 = 2\n", "a", "1 = 2"));
    CHECK(looked("Volume = -10\n", "volume", NULL));          /* keys are as written */
    char text[] = "volume = -10\0garbage\ntimezone = UTC\n";
    char v[16];
    CHECK(settings_lookup(text, sizeof(text) - 1, "timezone", v, sizeof(v)) && !strcmp(v, "UTC"));
    CHECK(!settings_lookup(text, sizeof(text) - 1, "volume", v, sizeof(v)));   /* NUL: no value */
    static char longline[600];
    memset(longline, 'x', sizeof(longline) - 1);
    memcpy(longline, "volume = ", 9);
    CHECK(looked(longline, "volume", NULL));                  /* a value too long */
    CHECK(settings_key_ok("music.volume") && settings_key_ok("a-b_c.9"));
    CHECK(!settings_key_ok("") && !settings_key_ok("a b") && !settings_key_ok("a=b") &&
          !settings_key_ok("x\xc5\xb8"));
    CHECK(settings_value_ok("") && settings_value_ok("/data/JA\xc5\xb8-Z"));
    CHECK(!settings_value_ok(" -10") && !settings_value_ok("a\nb") && !settings_value_ok("\x7f"));
    return true;
}

bool t_settings_edit(void)
{
    CHECK(edited("", "volume", "-10", "volume = -10\n"));
    CHECK(edited("# mine\nvolume = -10\nrtc = utc", "volume", "-3.5",
                 "# mine\nvolume = -3.5\nrtc = utc\n"));
    CHECK(edited("rtc = utc\r\n", "volume", "0", "rtc = utc\r\nvolume = 0\n"));
    CHECK(edited("volume = 1\nx\nvolume = 2\n", "volume", "3", "volume = 3\nx\n"));
    CHECK(edited("odd line\n\n# c\n", "a", "b", "odd line\n\n# c\na = b\n"));
    char out[8];
    size_t n;
    CHECK_ST(settings_edit("", 0, "volume", "-10", out, sizeof(out), &n), ERR_BUFFER_TOO_SMALL);
    CHECK_ST(settings_edit("", 0, "a b", "1", out, sizeof(out), &n), ERR_INVALID_ARGS);
    CHECK_ST(settings_edit("", 0, "a", "1\n2", out, sizeof(out), &n), ERR_INVALID_ARGS);
    return true;
}

#define SF "/s/etc/settings"

/* The file: made with its folder, read back, kept when nothing changes, a
 * left-over settings.new taken when the file itself is missing, never a
 * settings.new left behind. */
static bool settings_on_fat(struct ramdisk *rd)
{
    char v[SETTINGS_VALUE_MAX];
    CHECK_ST(settings_get(SF, "volume", v, sizeof(v)), ERR_NOT_FOUND);
    CHECK_ST(settings_set(SF, "volume", "-10"), OK);
    CHECK_ST(settings_get(SF, "volume", v, sizeof(v)), OK);
    CHECK(!strcmp(v, "-10"));
    CHECK_ST(fs_stat(SF ".new", NULL, NULL, NULL), ERR_NOT_FOUND);
    uint32_t writes = ramdisk_writes(rd);
    CHECK_ST(settings_set(SF, "volume", "-10"), OK);          /* the same: nothing written */
    CHECK_EQ(ramdisk_writes(rd), writes);
    CHECK_ST(settings_set(SF, "timezone", "Europe/London"), OK);
    CHECK_ST(settings_get(SF, "timezone", v, sizeof(v)), OK);
    CHECK(!strcmp(v, "Europe/London"));
    CHECK_ST(settings_get(SF, "volume", v, 3), ERR_BUFFER_TOO_SMALL);
    CHECK_ST(settings_set(SF, "volume", "a\nb"), ERR_INVALID_ARGS);
    /* A pull between the old file's unlink and the rename: only .new. */
    CHECK_ST(fs_rename(SF, SF ".new"), OK);
    CHECK_ST(settings_get(SF, "timezone", v, sizeof(v)), OK);
    CHECK(!strcmp(v, "Europe/London"));
    CHECK_ST(settings_set(SF, "volume", "-20"), OK);          /* .new is the settings first */
    CHECK_ST(settings_get(SF, "timezone", v, sizeof(v)), OK);
    CHECK(!strcmp(v, "Europe/London"));
    CHECK_ST(fs_stat(SF ".new", NULL, NULL, NULL), ERR_NOT_FOUND);
    /* A pull in the middle of writing .new: the file is still the one. */
    struct jfile f;
    size_t done;
    CHECK_ST(file_open(SF ".new", FS_WRITE | FS_CREATE, &f), OK);
    CHECK_ST(file_write(&f, 0, "volu", 4, &done), OK);
    file_close(&f);
    CHECK_ST(settings_get(SF, "volume", v, sizeof(v)), OK);
    CHECK(!strcmp(v, "-20"));
    CHECK_ST(settings_set(SF, "volume", "-30"), OK);          /* and the half .new goes */
    CHECK_ST(fs_stat(SF ".new", NULL, NULL, NULL), ERR_NOT_FOUND);
    CHECK_ST(settings_get(SF, "volume", v, sizeof(v)), OK);
    CHECK(!strcmp(v, "-30"));
    /* Too big a file is refused, not read in part. */
    CHECK_ST(file_open(SF, FS_WRITE | FS_TRUNCATE, &f), OK);
    static char big[SETTINGS_MAX + 2];
    memset(big, '#', sizeof(big));
    CHECK_ST(file_write(&f, 0, big, sizeof(big), &done), OK);
    file_close(&f);
    CHECK_ST(settings_get(SF, "volume", v, sizeof(v)), ERR_OUT_OF_RANGE);
    return true;
}

bool t_settings_file(void)
{
    static struct ramdisk disk;
    struct fatrun r;
    handle_t fs;
    if (!ramdisk_create(&disk, 16 * MIB_SECTORS) || !fat_start(&r, &disk, false))
        return false;
    CHECK_ST(jam_handle_duplicate(r.fs, RIGHT_SAME, &fs), OK);
    CHECK_ST(ns_mount("/s", fs), OK);
    bool ok = settings_on_fat(&disk);
    CHECK_ST(ns_unmount("/s"), OK);
    return fat_stop(&r) && ramdisk_destroy(&disk) && ok;
}
