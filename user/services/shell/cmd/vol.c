/* vol: the mixer's volumes (abi/idl/audioctl.idl). Alone: the master
 * volume and every stream (id, volume, state, underruns, frames heard,
 * name); `vol <id> <dB>` sets a stream's volume, `vol master <dB>` the
 * master. Volumes are attenuation only: 0 dB is the most, -96 dB and
 * below is silence. `hda gain` is the codec's own output level, below
 * the mixer. */
#include <idl/audioctl.h>
#include <mixer.h>
#include "sh.h"

#define SOON (5 * NS_PER_S)

static const char *vol_str(int32_t cb, char *buf, size_t size)
{
    if (cb <= -960)
        return "mute";
    char d[16];
    snprintf(buf, size, "%s dB", sh_db(cb, d, sizeof(d)));
    return buf;
}

static int list(handle_t ctl)
{
    uint32_t count = 0;
    int32_t master = 0;
    uint8_t raw[640];
    status_t st = audioctl_streams_until(ctl, now() + SOON, &count, &master, raw);
    if (st != OK) {
        sh_say("vol: the mixer doesn't answer: %s\n", status_str(st));
        return 1;
    }
    char a[24];
    sh_say("master %s\n", vol_str(master, a, sizeof(a)));
    if (!count) {
        sh_say("no streams\n");
        return 0;
    }
    static const char *const states[] = { "stopped", "playing", "idle" };
    sh_say("  id  volume     state    underruns  heard (s)  name\n");
    for (uint32_t i = 0; i < count && i < MIXER_MAX_STREAMS; i++) {
        struct mixer_stream_info e;
        memcpy(&e, raw + i * sizeof(e), sizeof(e));
        e.name[sizeof(e.name) - 1] = 0;
        uint64_t ms = e.played * 1000 / MIXER_RATE;
        sh_say("%4u  %-9s  %-7s  %9u  %5lu.%03lu  %s\n", e.id, vol_str(e.volume, a, sizeof(a)),
               e.state < 3 ? states[e.state] : "?", e.underruns, (unsigned long)(ms / 1000),
               (unsigned long)(ms % 1000), e.name);
    }
    return 0;
}

SH_CMD(vol)
{
    uint64_t id = 0;
    int32_t cb = 0;
    bool master = argc == 3 && !strcmp(argv[1], "master");
    if ((argc != 1 && argc != 3) ||
        (argc == 3 && ((!master && (!sh_parse_u64(argv[1], &id) || id > UINT32_MAX)) ||
                       !sh_parse_db(argv[2], &cb)))) {
        sh_tty("usage: vol [<id>|master <dB>]   (e.g. vol 3 -6, vol master -10; 0 dB is "
               "the most)\n");
        return 2;
    }
    handle_t ctl = sh_audio_ctl();
    if (!ctl) {
        sh_say("vol: no mixer\n");
        return 1;
    }
    if (argc == 1)
        return list(ctl);
    int32_t got = 0;
    status_t st = master ? audioctl_set_master_until(ctl, now() + SOON, cb, &got)
                         : audioctl_set_volume_until(ctl, now() + SOON, (uint32_t)id, cb, &got);
    char a[24];
    if (st == ERR_NOT_FOUND)
        sh_say("vol: no stream %lu (`vol` lists them)\n", (unsigned long)id);
    else if (st != OK)
        sh_say("vol: %s\n", status_str(st));
    else if (master)
        sh_say("vol: master %s\n", vol_str(got, a, sizeof(a)));
    else
        sh_say("vol: stream %lu %s\n", (unsigned long)id, vol_str(got, a, sizeof(a)));
    return st == OK ? 0 : 1;
}
