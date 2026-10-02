/* init's settings (<settings.h>, /data/etc/settings): the clock, and the
 * volumes to start with.
 *
 * The clock: at the start of shell mode, and again whenever /data comes,
 * init reads the real-time clock and sets the kernel's clock
 * (wallclock_set) to UTC: the RTC keeps either the zone's local time
 * (`rtc = local`, the default: Windows sets it so on a PC that runs both)
 * or UTC (`rtc = utc`), and the zone is `timezone` (default
 * Australia/Sydney), which the kernel also hands every program that asks
 * the time (<wallclock.h>). `rtc` may also name a zone (`rtc =
 * Australia/Sydney`): the RTC keeps that zone's time, whatever zone times
 * are shown in. Before /data is there (the first time) the defaults are
 * used. Once bin/sntp has set the clock from the network (WALLCLOCK_NET),
 * the network's time is better than any reading of the RTC: init then
 * gives the kernel the zone only and keeps the time (a stick pulled and
 * put back would otherwise put the RTC's error back for up to an hour).
 *
 * The volumes: `volume` to the mixer's master and `music.volume` to the
 * music player, each when it starts (again) and when /data comes, with a
 * short deadline: init's loop never waits long for a service.
 *
 * The first time /data has no settings file, init writes one with the
 * defaults and a comment on each key, so it can be found and edited on
 * the Mac. */
#include <idl/audioctl.h>
#include <idl/music.h>
#include <mixmath.h>
#include <settings.h>
#include <wallclock.h>
#include "init.h"

#define CALL_WAIT NS_PER_S

static const char template_text[] =
    "# Jam OS settings: one `key = value` a line; a line starting with # is a comment.\n"
    "# Jam OS reads them when the stick is mounted and writes them when `vol`,\n"
    "# `music` and `date -z` change something. Edit them on any computer: they\n"
    "# count from the next boot.\n"
    "#\n"
    "# timezone: the zone times are shown and files are dated in (Australia/Sydney,\n"
    "#   Australia/Perth, Pacific/Auckland, Europe/London, UTC, UTC+05:30, ...)\n"
    "timezone = " TZ_DEFAULT "\n"
    "# rtc: what the PC's real-time clock keeps: local (the zone's time, as Windows\n"
    "#   sets it), utc (as Linux and macOS do), or a zone (Windows set to another one)\n"
    "rtc = local\n"
    "# volume: the master volume in dB (0 is the most, -96 silence); music.volume:\n"
    "#   the music player's; music.folder: what `music start` plays without a folder\n";

/* key's value from the settings file, or dflt if there is none (or no
 * /data). */
static void setting(const char *key, const char *dflt, char *out, size_t cap)
{
    if (settings_get(SETTINGS_FILE, key, out, cap) != OK)
        snprintf(out, cap, "%s", dflt);
}

/* The clock came from the network: only the zone changes, the time stays. */
static void zone_only(struct wall_clock *w, const struct tz *zone)
{
    w->flags = WALLCLOCK_NET;   /* still the network's */
    w->reserved = 0;
    memset(w->zone, 0, sizeof(w->zone));
    snprintf(w->zone, sizeof(w->zone), "%s", zone->name);
    status_t st = jam_wallclock_set(shell_root(), w);
    char when[48];
    time_format_iso(w->utc_ns / NS_PER_S, zone, when, sizeof(when));
    printf("init: the clock: %s, kept (set from the network)%s%s\n", when,
           st == OK ? "" : ": the zone not set, ", st == OK ? "" : status_str(st));
}

void settings_clock(void)
{
    char zone_name[SETTINGS_VALUE_MAX], rtc[16];
    setting("timezone", TZ_DEFAULT, zone_name, sizeof(zone_name));
    setting("rtc", "local", rtc, sizeof(rtc));
    struct tz zone;
    if (!tz_parse(zone_name, &zone)) {
        printf("init: settings: timezone = %s is not a zone libos knows: %s instead\n",
               zone_name, TZ_DEFAULT);
        tz_parse(TZ_DEFAULT, &zone);
    }
    struct wall_clock now_w;
    if (jam_wallclock_get(&now_w) == OK && (now_w.flags & WALLCLOCK_NET)) {
        zone_only(&now_w, &zone);
        return;
    }
    /* The zone the RTC keeps: the system's (local), UTC, or one named. */
    struct tz rtc_zone = zone;
    if (!strcmp(rtc, "utc") || !strcmp(rtc, "UTC"))
        tz_parse("UTC", &rtc_zone);
    else if (strcmp(rtc, "local") && !tz_parse(rtc, &rtc_zone))
        printf("init: settings: rtc = %s is not local, utc or a zone: local\n", rtc);
    bool rtc_utc = rtc_zone.std_min == 0 && rtc_zone.dst_min == 0;
    struct rtc_time r;
    status_t st = jam_rtc_read(shell_root(), &r);
    if (st != OK) {
        printf("init: the real-time clock can't be read (%s): the clock is not set\n",
               status_str(st));
        return;
    }
    int64_t s = civil_days(r.year, r.month, r.day) * 86400 + r.hour * 3600 + r.minute * 60 +
                r.second;
    struct wall_clock w = { 0 };
    w.utc_ns = tz_local_to_utc(&rtc_zone, s) * NS_PER_S;
    w.uptime_ns = r.uptime_ns;
    snprintf(w.zone, sizeof(w.zone), "%s", zone.name);
    st = jam_wallclock_set(shell_root(), &w);
    char when[48];
    time_format_iso(w.utc_ns / NS_PER_S, &zone, when, sizeof(when));
    printf("init: the clock: %s (the RTC keeps %s time)%s%s\n", when,
           rtc_utc ? "UTC" : strcmp(rtc_zone.name, zone.name) ? rtc_zone.name : "local",
           st == OK ? "" : ": not set, ", st == OK ? "" : status_str(st));
}

/* A volume setting (dB) as centibels; false if it isn't there or isn't one. */
static bool volume(const char *key, int32_t *cb)
{
    char v[32];
    if (settings_get(SETTINGS_FILE, key, v, sizeof(v)) != OK)
        return false;
    if (mix_parse_db(v, cb))
        return true;
    printf("init: settings: %s = %s is not a volume in dB\n", key, v);
    return false;
}

void settings_master(handle_t audioctl)
{
    int32_t cb, got;
    if (!audioctl || !volume("volume", &cb))
        return;
    status_t st = audioctl_set_master_until(audioctl, now() + CALL_WAIT, cb, &got);
    if (st != OK)
        printf("init: settings: the mixer didn't take the volume (%s)\n", status_str(st));
}

void settings_music(handle_t music)
{
    int32_t cb, got;
    if (!music || !volume("music.volume", &cb))
        return;
    status_t st = music_set_volume_until(music, now() + CALL_WAIT, cb, &got);
    if (st != OK)
        printf("init: settings: the music player didn't take its volume (%s)\n", status_str(st));
}

void settings_first_file(void)
{
    char v[8];
    if (settings_get(SETTINGS_FILE, "rtc", v, sizeof(v)) != ERR_NOT_FOUND ||
        fs_stat(SETTINGS_FILE, NULL, NULL, NULL) != ERR_NOT_FOUND)
        return;   /* there is a file (or /data can't say) */
    status_t st = settings_write(SETTINGS_FILE, template_text, sizeof(template_text) - 1);
    printf("init: settings: %s %s\n", SETTINGS_FILE, st == OK ? "written with the defaults"
                                                              : status_str(st));
}
