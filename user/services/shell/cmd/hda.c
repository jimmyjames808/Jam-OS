/* hda: the HD Audio controller's codecs and their widget graphs, as the
 * driver (drivers/hda) reads them now: the same lines it printed to the
 * log when it started (abi/idl/hda.idl). Then the path to the headphones
 * the driver set up, what is set on each of its nodes (hda.info), and the
 * gain it plays at. `hda gain [dB]` shows or sets the gain (hda.set_gain:
 * the driver rounds to the amp's step and clamps to its range, never
 * above 0 dB); `hda bits [n]` the largest sample size (hda.set_bits);
 * `hda jacks` each jack's state as the driver tracks it (hda.jacks: plugged
 * in or not, and whether unsolicited responses or polling found it), the
 * same lines `hda` ends with.
 *
 * The driver is reached through the mixer (audioctl.device), never
 * devmgr: devmgr hands the driver's own channel to the mixer alone
 * (<devmgr.h>, "exclusive"), and the mixer hands out query channels,
 * which answer everything here but can't open the output stream the
 * mixer plays through (abi/idl/hda.idl, `query`). */
#include <idl/audioctl.h>
#include <idl/hda.h>
#include "sh.h"

#define DUMP_WAIT   (10 * NS_PER_S)   /* a codec's dump is a few hundred verbs */
#define DUMP_MAX    (64 * 1024)       /* the driver's dump buffer */
#define DEVICE_WAIT (15 * NS_PER_S)   /* the mixer asks devmgr and each driver in turn */
#define MAX_DEVICES 8u                /* HD Audio drivers asked for at most */

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

/* hda.jacks's lines, each with "hda: jack " before it. */
static void print_jacks(handle_t ch)
{
    uint32_t count = 0, state = 0, changes = 0;
    uint8_t pins[16], states[16];
    char *text = malloc(1024);
    if (!text) {
        sh_say("hda: jacks: no memory\n");
        return;
    }
    status_t st = hda_jacks_until(ch, now() + DUMP_WAIT, &count, &state, &changes, pins, states,
                                  (uint8_t *)text);
    if (st != OK) {
        sh_say("hda: jacks: %s\n", status_str(st));
        free(text);
        return;
    }
    text[1023] = 0;
    for (char *line = text; *line;) {
        char *end = strchr(line, '\n');
        if (end)
            *end = 0;
        sh_say("hda: jack %s\n", line);
        if (!end)
            break;
        line = end + 1;
    }
    free(text);
}

static int jacks_cmd(void)
{
    handle_t ch = sh_hda();
    if (ch == HANDLE_INVALID) {
        sh_say("hda: no HD Audio driver with a path to a jack\n");
        return 1;
    }
    print_jacks(ch);
    jam_handle_close(ch);
    return 0;
}

static void print_gain(handle_t ch)
{
    int32_t gain = 0, min = 0, max = 0;
    uint32_t step = 0;
    status_t st = hda_get_gain_until(ch, now() + DUMP_WAIT, &gain, &step, &min, &max);
    say_gain(st, gain, step, min, max);
}

/* The n-th HD Audio driver's query channel, from the mixer: OK, or
 * ERR_NOT_FOUND past the last (or with no mixer), or why not. */
static status_t device(uint32_t n, handle_t *ch)
{
    handle_t ctl = sh_audio_ctl();
    if (!ctl)
        return ERR_NOT_FOUND;
    return audioctl_device_until(ctl, now() + DEVICE_WAIT, n, ch);
}

handle_t sh_hda(void)
{
    for (uint32_t n = 0; n < MAX_DEVICES; n++) {
        handle_t ch;
        status_t st = device(n, &ch);
        if (st == ERR_NOT_FOUND)
            break;
        if (st != OK)
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

/* The sizes in a P_PCM word: "16, 20, 24". */
static void pcm_sizes(uint32_t pcm, char *buf, size_t size)
{
    static const unsigned bits[] = { 8, 16, 20, 24, 32 };
    size_t len = 0;
    buf[0] = 0;
    for (unsigned i = 0; i < 5; i++)
        if (pcm & (1u << (16 + i)))
            len += (size_t)snprintf(buf + len, size - len, "%s%u", len ? ", " : "", bits[i]);
}

/* `hda bits [16|20|24|32]`: the largest sample size the output uses from
 * the next stream on (hda.set_bits). */
static int bits_cmd(int argc, char **argv)
{
    uint64_t bits = 0;
    if (argc == 3 && !sh_parse_u64(argv[2], &bits))
        bits = 1;
    if (argc > 3 || (argc == 3 && bits != 16 && bits != 20 && bits != 24 && bits != 32)) {
        sh_tty("usage: hda bits [16|20|24|32]   (the largest sample size the output uses)\n");
        return 2;
    }
    handle_t ch = sh_hda();
    if (ch == HANDLE_INVALID) {
        sh_say("hda: no HD Audio driver with a path to a jack\n");
        return 1;
    }
    uint32_t cap = 0, pcm = 0;
    status_t st = hda_set_bits_until(ch, now() + DUMP_WAIT, (uint32_t)bits, &cap, &pcm);
    jam_handle_close(ch);
    if (st != OK) {
        sh_say("hda: bits: %s\n", status_str(st));
        return 1;
    }
    char sizes[32];
    pcm_sizes(pcm, sizes, sizeof(sizes));
    sh_say("hda: bits: at most %u; the DAC takes %s (the mixer uses the largest from its next "
           "open)\n", cap, sizes[0] ? sizes : "none");
    return 0;
}

/* Each HD Audio driver's dump, path, gain and jacks, as the mixer finds
 * them; returns how many there were. */
static unsigned dump_each(void)
{
    unsigned found = 0;
    for (uint32_t n = 0; n < MAX_DEVICES && !sh_interrupted(); n++) {
        handle_t ch, text;
        uint32_t len = 0, codecs = 0;
        status_t st = device(n, &ch);
        if (st == ERR_NOT_FOUND)
            break;
        found++;
        if (st == OK && (st = hda_dump_until(ch, now() + DUMP_WAIT, &text, &len, &codecs)) != OK)
            jam_handle_close(ch);
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
        print_jacks(ch);
        jam_handle_close(ch);
    }
    return found;
}

SH_CMD(hda)
{
    if (argc >= 2 && !strcmp(argv[1], "gain"))
        return gain_cmd(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "bits"))
        return bits_cmd(argc, argv);
    if (argc == 2 && !strcmp(argv[1], "jacks"))
        return jacks_cmd();
    if (argc != 1) {
        sh_tty("usage: hda [gain [dB] | bits [16|20|24|32] | jacks]\n");
        return 2;
    }
    if (!sh_audio_ctl()) {
        sh_say("hda: no mixer (the sound card is reached through it)\n");
        return 1;
    }
    if (!dump_each()) {
        sh_say("hda: no HD Audio driver bound (the mixer finds none through devmgr)\n");
        return 1;
    }
    return 0;
}
