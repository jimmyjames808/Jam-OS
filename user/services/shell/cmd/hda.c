/* hda: the HD Audio controller's codecs and their widget graphs, as the
 * driver (drivers/hda) reads them now: the same lines it printed to the
 * log when it started (abi/idl/hda.idl). Then the path to the headphones
 * the driver set up, what is set on each of its nodes (hda.info), and the
 * gain it plays at. `hda gain [dB]` shows or sets the gain (hda.set_gain:
 * the driver rounds to the amp's step and clamps to its range, never
 * above 0 dB). */
#include <devmgr.h>
#include <idl/hda.h>
#include "sh.h"

#define DUMP_WAIT (10 * NS_PER_S)   /* a codec's dump is a few hundred verbs */
#define DUMP_MAX  (64 * 1024)       /* the driver's dump buffer */

/* hda.info's line: the path the driver set up, or why there is none. */
static void print_path(handle_t ch)
{
    uint32_t codec, pin, dac, pcm, formats, amp, jack, count;
    uint8_t nodes[8], text[240];
    status_t st = hda_info_until(ch, now() + DUMP_WAIT, &codec, &pin, &dac, &pcm, &formats, &amp,
                                 &jack, &count, nodes, text);
    if (st != OK) {
        sh_say("hda: path: %s\n", status_str(st));
        return;
    }
    text[sizeof(text) - 1] = 0;
    sh_say("hda: path: %s\n", (const char *)text);
}

/* "hda: gain -30.0 dB (step 47; -65.3 to 0.0 dB)", or why there is none. */
static void say_gain(status_t st, int32_t gain, uint32_t step, int32_t min, int32_t max)
{
    char a[16], b[16], c[16];
    if (st == ERR_NOT_FOUND)
        sh_say("hda: gain: no path was set up\n");
    else if (st == ERR_NOT_SUPPORTED)
        sh_say("hda: gain: no amp on the path has gain steps (it plays at 0 dB)\n");
    else if (st != OK)
        sh_say("hda: gain: %s\n", status_str(st));
    else
        sh_say("hda: gain %s dB (step %u; %s to %s dB), heard only while a stream plays\n",
               sh_db(gain, a, sizeof(a)), step, sh_db(min, b, sizeof(b)), sh_db(max, c, sizeof(c)));
}

static void print_gain(handle_t ch)
{
    int32_t gain = 0, min = 0, max = 0;
    uint32_t step = 0;
    status_t st = hda_get_gain_until(ch, now() + DUMP_WAIT, &gain, &step, &min, &max);
    say_gain(st, gain, step, min, max);
}

handle_t sh_hda(void)
{
    handle_t dm = sh_devmgr();
    for (uint32_t n = 0; dm && n < 32; n++) {
        struct devmgr_rep r;
        handle_t ch;
        uint32_t nh = 0;
        status_t st = devmgr_call(dm, DEVMGR_GET_SERVICE, 0xffff, 0xffff, n, &r, &ch, 1, &nh,
                                  now() + 5 * NS_PER_S);
        if (st == ERR_NOT_FOUND)
            break;
        if (st != OK || nh != 1)
            continue;
        uint32_t codec, pin = 0, dac, pcm, formats, amp, jack, count;
        uint8_t nodes[8], text[240];
        st = hda_info_until(ch, now() + DUMP_WAIT, &codec, &pin, &dac, &pcm, &formats, &amp,
                            &jack, &count, nodes, text);
        if (st == OK && pin)
            return ch;
        jam_handle_close(ch);
    }
    return HANDLE_INVALID;
}

static int gain_cmd(int argc, char **argv)
{
    int32_t cb = 0;
    if (argc > 3 || (argc == 3 && !sh_parse_db(argv[2], &cb))) {
        sh_tty("usage: hda gain [dB]   (e.g. hda gain -20; 0 dB is the most)\n");
        return 2;
    }
    handle_t ch = sh_hda();
    if (ch == HANDLE_INVALID) {
        sh_say("hda: no HD Audio driver with a path to a jack\n");
        return 1;
    }
    int32_t gain = 0, min = 0, max = 0;
    uint32_t step = 0;
    status_t st = argc == 3
        ? hda_set_gain_until(ch, now() + DUMP_WAIT, cb, &gain, &step, &min, &max)
        : hda_get_gain_until(ch, now() + DUMP_WAIT, &gain, &step, &min, &max);
    jam_handle_close(ch);
    say_gain(st, gain, step, min, max);
    return st == OK ? 0 : 1;
}

/* Ask each running PCI driver in turn for hda.dump; others answer
 * ERR_NOT_SUPPORTED. Prints each dump; returns how many answered. */
static unsigned dump_each(handle_t dm)
{
    unsigned found = 0;
    for (uint32_t n = 0; n < 32 && !sh_interrupted(); n++) {
        struct devmgr_rep r;
        handle_t ch, text;
        uint32_t nh = 0, len = 0, codecs = 0;
        status_t st = devmgr_call(dm, DEVMGR_GET_SERVICE, 0xffff, 0xffff, n, &r, &ch, 1, &nh,
                                  now() + 5 * NS_PER_S);
        if (st == ERR_NOT_FOUND)
            break;
        if (st != OK || nh != 1)
            continue;
        st = hda_dump_until(ch, now() + DUMP_WAIT, &text, &len, &codecs);
        if (st != OK)
            jam_handle_close(ch);
        if (st == ERR_NOT_SUPPORTED)
            continue;   /* another driver's service */
        found++;
        if (st != OK) {
            sh_say("hda: %s\n", status_str(st));
            continue;
        }
        char *buf = len && len <= DUMP_MAX ? malloc(len) : NULL;
        if (buf && jam_vmo_read(text, 0, buf, len) == OK)
            sh_put(buf, len);
        else
            sh_say("hda: can't read the dump (%u bytes)\n", len);
        free(buf);
        jam_handle_close(text);
        print_path(ch);
        print_gain(ch);
        jam_handle_close(ch);
    }
    return found;
}

SH_CMD(hda)
{
    if (argc >= 2 && !strcmp(argv[1], "gain"))
        return gain_cmd(argc, argv);
    if (argc != 1) {
        sh_tty("usage: hda [gain [dB]]\n");
        return 2;
    }
    handle_t dm = sh_devmgr();
    if (!dm) {
        sh_say("hda: no devmgr\n");
        return 1;
    }
    if (!dump_each(dm)) {
        sh_say("hda: no HD Audio driver bound (devmgr has none that answers hda)\n");
        return 1;
    }
    return 0;
}
