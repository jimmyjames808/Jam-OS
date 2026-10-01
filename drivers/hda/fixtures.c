/* hda: fixtures, the codecs the path finder (path.c) is checked against
 * every time the driver starts, and the parser that rebuilds a struct
 * codec from dump lines.
 *
 * A fixture is a codec's dump as dump.c prints it, pasted from a log:
 * QEMU's three codecs (the ones tools/hda-test.sh gives the driver) and
 * the PC's Realtek ALC897 from its first boot with the probe. Each has the
 * path the finder must give, and the output verbs.c makes of it (which
 * amp is the volume, its default step at -30 dB, its range, and that
 * set_gain clamps to it, and whether an amp on the path can mute it); a
 * few variations (the ALC897's rear line-out playing and front jack not
 * described, QEMU's output with no connection or no amps) check the
 * fallbacks. A codec from another board is added by pasting its
 * `hda` lines here with the path it should get.
 *
 * The parser reads only what the finder and the programming use: each
 * widget's capabilities, amplifier capabilities, power state and
 * connection list (with the selected entry, the `*`), and each pin's
 * configuration default, capabilities, pin control and EAPD. Other lines
 * (the controller, the summaries) are skipped. At start the driver also
 * parses its own dump of each live codec back and checks the path comes
 * out the same (hda_path_roundtrip), which keeps this parser and dump.c
 * in step. */
#include "hda.h"

/* ---- the fixtures: dump lines as the driver logged them ------------------------ */

/* QEMU's hda-output (1af4:0012), codec 1 on tools/hda-test.sh's intel-hda. */
static const char qemu_output[] =
    "codec 1: 1af4:0012 rev 0x100101 subsys 1af4:0012, 1 function group(s); audio group "
        "at node 01\n"
    "codec 1 afg 01: widgets 02-03 pwr D0/D0 (supports 0x0) gpio 0x0 caps 0x808 pcm 16b "
        "16,22,32,44.1,48,88.2,96k\n"
    "c1 02 dac      caps 0000001d 2ch out-amp 0-74 x1.00dB 0dB@74 +mute now 74 stream 0 "
        "ch 0 fmt 0x0011 pcm 16b 16,22,32,44.1,48,88.2,96k\n"
    "c1 03 pin      caps 00400101 2ch conn 02\n"
    "c1 03   cfg 00004010: jack ext green line-out jack? as1 sq0 | caps 00000010 out | "
        "ctl 40 out vref=hiz\n"
    ;

/* QEMU's hda-duplex (1af4:0022), codec 0 there. */
static const char qemu_duplex[] =
    "codec 0: 1af4:0022 rev 0x100101 subsys 1af4:0022, 1 function group(s); audio group "
        "at node 01\n"
    "codec 0 afg 01: widgets 02-05 pwr D0/D0 (supports 0x0) gpio 0x0 caps 0x808 pcm 16b "
        "16,22,32,44.1,48,88.2,96k\n"
    "c0 02 dac      caps 0000001d 2ch out-amp 0-74 x1.00dB 0dB@74 +mute now 74 stream 0 "
        "ch 0 fmt 0x0011 pcm 16b 16,22,32,44.1,48,88.2,96k\n"
    "c0 03 pin      caps 00400101 2ch conn 02\n"
    "c0 03   cfg 00004010: jack ext green line-out jack? as1 sq0 | caps 00000010 out | "
        "ctl 40 out vref=hiz\n"
    "c0 04 adc      caps 0010011b 2ch in-amp 0-74 x1.00dB 0dB@74 +mute now 0 conn 05 "
        "stream 0 ch 0 fmt 0x0011 pcm 16b 16,22,32,44.1,48,88.2,96k\n"
    "c0 05 pin      caps 00400001 2ch\n"
    "c0 05   cfg 00805020: jack ext red line-in jack? as2 sq0 | caps 00000020 in | ctl 20 "
        "in vref=hiz\n"
    ;

/* QEMU's hda-micro (1af4:0032), on the ich9-intel-hda. */
static const char qemu_micro[] =
    "codec 0: 1af4:0032 rev 0x100101 subsys 1af4:0032, 1 function group(s); audio group "
        "at node 01\n"
    "codec 0 afg 01: widgets 02-05 pwr D0/D0 (supports 0x0) gpio 0x0 caps 0x808 pcm 16b "
        "16,22,32,44.1,48,88.2,96k\n"
    "c0 02 dac      caps 0000001d 2ch out-amp 0-74 x1.00dB 0dB@74 +mute now 74 stream 0 "
        "ch 0 fmt 0x0011 pcm 16b 16,22,32,44.1,48,88.2,96k\n"
    "c0 03 pin      caps 00400101 2ch conn 02\n"
    "c0 03   cfg 00104010: jack ext green speaker jack? as1 sq0 | caps 00000010 out | ctl "
        "40 out vref=hiz\n"
    "c0 04 adc      caps 0010011b 2ch in-amp 0-74 x1.00dB 0dB@74 +mute now 0 conn 05 "
        "stream 0 ch 0 fmt 0x0011 pcm 16b 16,22,32,44.1,48,88.2,96k\n"
    "c0 05 pin      caps 00400001 2ch\n"
    "c0 05   cfg 00a05020: jack ext red mic jack? as2 sq0 | caps 00000020 in | ctl 20 in "
        "vref=hiz\n"
    ;

/* The PC's codec: Realtek ALC897 on the ASUS board (subsystem 1043:8841),
 * from its boot log of 2026-10-01 (boot-0011). */
static const char pc_alc897[] =
    "codec 0: 10ec:0897 rev 0x100500 subsys 1043:8841, 1 function group(s); audio group "
        "at node 01 (unsolicited-capable)\n"
    "codec 0 afg 01: widgets 02-26 pwr D0/D0 (supports 0xc000000f) gpio 0x40000005 caps "
        "0x10f0f pcm 16,20,24b 32,44.1,48,88.2,96,192k\n"
    "c0 02 dac      caps 0000041d 2ch pwr D0/D0 out-amp 0-87 x0.75dB 0dB@87 now 87 stream "
        "0 ch 0 fmt 0x0031 pcm 16,20,24b 44.1,48,96,192k\n"
    "c0 03 dac      caps 0000041d 2ch pwr D0/D0 out-amp 0-87 x0.75dB 0dB@87 now 87 stream "
        "0 ch 0 fmt 0x0031 pcm 16,20,24b 44.1,48,96,192k\n"
    "c0 04 dac      caps 0000041d 2ch pwr D0/D0 out-amp 0-87 x0.75dB 0dB@87 now 87 stream "
        "0 ch 0 fmt 0x0031 pcm 16,20,24b 44.1,48,96,192k\n"
    "c0 05 dac      caps 0000041d 2ch pwr D0/D0 out-amp 0-87 x0.75dB 0dB@87 now 87 stream "
        "0 ch 0 fmt 0x0031 pcm 16,20,24b 44.1,48,96,192k\n"
    "c0 06 dac      caps 00000611 2ch digital pwr D0/D0 stream 0 ch 0 fmt 0x0031 pcm "
        "16,20,24b 32,44.1,48,88.2,96,192k\n"
    "c0 07 adc      caps 0010051b 2ch pwr D0/D0 in-amp 0-63 x0.75dB 0dB@23 +mute now m23 "
        "conn 12 stream 0 ch 0 fmt 0x0031 pcm 16,20,24b 44.1,48,96,192k\n"
    "c0 08 adc      caps 0010051b 2ch pwr D0/D0 in-amp 0-63 x0.75dB 0dB@23 +mute now m23 "
        "conn 23 stream 0 ch 0 fmt 0x0031 pcm 16,20,24b 44.1,48,96,192k\n"
    "c0 09 adc      caps 0010051b 2ch pwr D0/D0 in-amp 0-63 x0.75dB 0dB@23 +mute now m23 "
        "conn 22 stream 0 ch 0 fmt 0x0031 pcm 16,20,24b 44.1,48,96,192k\n"
    "c0 0a adc      caps 00100711 2ch digital pwr D0/D0 conn 1f stream 0 ch 0 fmt 0x0031 "
        "pcm 16,20,24b 44.1,48,96,192k\n"
    "c0 0b mixer    caps 0020010b 2ch in-amp 0-31 x1.50dB 0dB@23 +mute now m23 m23 m23 "
        "m23 m23 m23 m23 m23 m23 m23 conn 18 19 1a 1b 1c 1d 14 15 16 17\n"
    "c0 0c mixer    caps 0020010b 2ch in-amp 0-0 x0.25dB 0dB@0 +mute now 0 m0 conn 02 0b\n"
    "c0 0d mixer    caps 0020010b 2ch in-amp 0-0 x0.25dB 0dB@0 +mute now 0 m0 conn 03 0b\n"
    "c0 0e mixer    caps 0020010b 2ch in-amp 0-0 x0.25dB 0dB@0 +mute now 0 m0 conn 04 0b\n"
    "c0 0f mixer    caps 0020010b 2ch in-amp 0-0 x0.25dB 0dB@0 +mute now 0 m0 conn 05 0b\n"
    "c0 10 dac      caps 00000611 2ch digital pwr D0/D0 stream 0 ch 0 fmt 0x0031 pcm "
        "16,20,24b 32,44.1,48,88.2,96,192k\n"
    "c0 11 pin      caps 00400781 2ch digital unsol pwr D0/D0 conn 10\n"
    "c0 11   cfg 40370040: none ext colour? cd analog as4 sq0 | caps 00000010 out | ctl "
        "40 out vref=hiz | unsol off tag 0\n"
    "c0 12 pin      caps 00400401 2ch pwr D0/D0\n"
    "c0 12   cfg 411111f0: none ext-rear black speaker 1/8in as15 sq0 no-detect | caps "
        "00000020 in | ctl 00 vref=hiz\n"
    "c0 13 vendor   caps 00f00000 1ch\n"
    "c0 14 pin      caps 0040058d 2ch unsol pwr D0/D0 out-amp 0-0 x0.25dB 0dB@0 +mute now "
        "m0 conn 0c\n"
    "c0 14   cfg 01014010: jack ext-rear green line-out 1/8in as1 sq0 | caps 0001003e out "
        "in hp presence trigger eapd | ctl 20 in vref=hiz eapd=off | unsol off tag 0\n"
    "c0 15 pin      caps 0040058d 2ch unsol pwr D0/D0 out-amp 0-0 x0.25dB 0dB@0 +mute now "
        "m0 conn 0d\n"
    "c0 15   cfg 01011012: jack ext-rear black line-out 1/8in as1 sq2 | caps 00000036 out "
        "in presence trigger | ctl 20 in vref=hiz | unsol off tag 0\n"
    "c0 16 pin      caps 0040058d 2ch unsol pwr D0/D0 out-amp 0-0 x0.25dB 0dB@0 +mute now "
        "m0 conn 0e\n"
    "c0 16   cfg 01016011: jack ext-rear orange line-out 1/8in as1 sq1 | caps 00000036 "
        "out in presence trigger | ctl 20 in vref=hiz | unsol off tag 0\n"
    "c0 17 pin      caps 0040058d 2ch unsol pwr D0/D0 out-amp 0-0 x0.25dB 0dB@0 +mute now "
        "m0 conn 0f\n"
    "c0 17   cfg 411111f0: none ext-rear black speaker 1/8in as15 sq0 no-detect | caps "
        "00000036 out in presence trigger | ctl 20 in vref=hiz | unsol off tag 0\n"
    "c0 18 pin      caps 0040058f 2ch unsol pwr D0/D0 out-amp 0-0 x0.25dB 0dB@0 +mute now "
        "m0 in-amp 0-3 x10.00dB 0dB@0 now 0 conn 0c* 0d 0e 0f 26\n"
    "c0 18   cfg 01a19030: jack ext-rear pink mic 1/8in as3 sq0 | caps 00003736 out in "
        "presence trigger vref=hiz,50,gnd,80,100 | ctl 20 in vref=hiz | unsol off tag 0\n"
    "c0 19 pin      caps 0040058f 2ch unsol pwr D0/D0 out-amp 0-0 x0.25dB 0dB@0 +mute now "
        "m0 in-amp 0-3 x10.00dB 0dB@0 now 0 conn 0c* 0d 0e 0f 26\n"
    "c0 19   cfg 02a19040: jack ext-front pink mic 1/8in as4 sq0 | caps 0000373e out in "
        "hp presence trigger vref=hiz,50,gnd,80,100 | ctl 20 in vref=hiz | unsol off tag 0\n"
    "c0 1a pin      caps 0040058f 2ch unsol pwr D0/D0 out-amp 0-0 x0.25dB 0dB@0 +mute now "
        "m0 in-amp 0-3 x10.00dB 0dB@0 now 0 conn 0c* 0d 0e 0f 26\n"
    "c0 1a   cfg 0181303f: jack ext-rear blue line-in 1/8in as3 sq15 | caps 00003736 out "
        "in presence trigger vref=hiz,50,gnd,80,100 | ctl 20 in vref=hiz | unsol off tag 0\n"
    "c0 1b pin      caps 0040058f 2ch unsol pwr D0/D0 out-amp 0-0 x0.25dB 0dB@0 +mute now "
        "m0 in-amp 0-3 x10.00dB 0dB@0 now 0 conn 0c* 0d 0e 0f 26\n"
    "c0 1b   cfg 02214020: jack ext-front green hp-out 1/8in as2 sq0 | caps 0001373e out "
        "in hp presence trigger eapd vref=hiz,50,gnd,80,100 | ctl 20 in vref=hiz eapd=off | "
        "unsol off tag 0\n"
    "c0 1c pin      caps 00400481 2ch unsol pwr D0/D0\n"
    "c0 1c   cfg 411111f0: none ext-rear black speaker 1/8in as15 sq0 no-detect | caps "
        "00000020 in | ctl 20 in vref=hiz | unsol off tag 0\n"
    "c0 1d pin      caps 00400400 1ch pwr D0/D0\n"
    "c0 1d   cfg 4025d601: none ext c13 hp-out optical as0 sq1 | caps 00000020 in | ctl "
        "20 in vref=hiz\n"
    "c0 1e pin      caps 00400781 2ch digital unsol pwr D0/D0 conn 06\n"
    "c0 1e   cfg 01456150: jack ext-rear orange spdif-out optical as5 sq0 no-detect | "
        "caps 00000010 out | ctl 40 out vref=hiz | unsol off tag 0\n"
    "c0 1f pin      caps 00400681 2ch digital unsol pwr D0/D0\n"
    "c0 1f   cfg 411111f0: none ext-rear black speaker 1/8in as15 sq0 no-detect | caps "
        "00000020 in | ctl 20 in vref=hiz | unsol off tag 0\n"
    "c0 20 vendor   caps 00f00040 1ch\n"
    "c0 21 vendor   caps 00f00000 1ch\n"
    "c0 22 mixer    caps 0020010b 2ch in-amp 0-0 x0.25dB 0dB@0 +mute now m0 m0 m0 m0 m0 "
        "m0 m0 m0 m0 m0 conn 18 19 1a 1b 1c 1d 14 15 16 17 0b 12\n"
    "c0 23 mixer    caps 0020010b 2ch in-amp 0-0 x0.25dB 0dB@0 +mute now m0 m0 m0 m0 m0 "
        "m0 m0 m0 m0 m0 conn 18 19 1a 1b 1c 1d 14 15 16 17 0b\n"
    "c0 24 vendor   caps 00f00000 1ch\n"
    "c0 25 dac      caps 0000041d 2ch pwr D0/D0 out-amp 0-87 x0.75dB 0dB@87 now 87 stream "
        "0 ch 0 fmt 0x0031 pcm 16,20,24b 44.1,48,96,192k\n"
    "c0 26 mixer    caps 0020010b 2ch in-amp 0-0 x0.25dB 0dB@0 +mute now 0 m0 conn 25 0b\n"
    ;

/* ---- the parser ---------------------------------------------------------------- */

/* The rest of one line, and the token last taken from it. */
struct line {
    const char *p, *end;
    const char *t;          /* the token */
    size_t      n;          /* its length */
};

/* The next space-separated token into l->t / l->n; false at the end. */
static bool next(struct line *l)
{
    while (l->p < l->end && *l->p == ' ')
        l->p++;
    if (l->p == l->end)
        return false;
    l->t = l->p;
    while (l->p < l->end && *l->p != ' ')
        l->p++;
    l->n = (size_t)(l->p - l->t);
    return true;
}

static bool is(const struct line *l, const char *word)
{
    size_t i = 0;
    while (i < l->n && word[i] && l->t[i] == word[i])
        i++;
    return i == l->n && !word[i];
}

static int digit(char ch, unsigned base)
{
    unsigned d = ch >= '0' && ch <= '9' ? (unsigned)(ch - '0')
               : ch >= 'a' && ch <= 'f' ? (unsigned)(ch - 'a' + 10) : 99;
    return d < base ? (int)d : -1;
}

/* n digits of s in base into *out (at most 8 of them). */
static bool number(const char *s, size_t n, unsigned base, uint32_t *out)
{
    uint32_t v = 0;
    if (n == 0 || n > 8)
        return false;
    for (size_t i = 0; i < n; i++) {
        int d = digit(s[i], base);
        if (d < 0)
            return false;
        v = v * base + (uint32_t)d;
    }
    *out = v;
    return true;
}

/* The next token as a hex number, `skip` characters dropped from its end. */
static bool next_hex(struct line *l, size_t skip, uint32_t *out)
{
    return next(l) && l->n > skip && number(l->t, l->n - skip, 16, out);
}

/* The index of ch in the token from `from` on, or l->n. */
static size_t find(const struct line *l, size_t from, char ch)
{
    while (from < l->n && l->t[from] != ch)
        from++;
    return from;
}

/* Amplifier capabilities from "0-87 x0.75dB 0dB@87 [+mute]" (dump.c's
 * add_ampcaps): steps, step size in hundredths of a dB, 0 dB offset. */
static bool ampcaps(struct line *l, uint32_t *out)
{
    uint32_t steps, whole, frac, offset;
    size_t dash, dot, at;
    if (!next(l) || (dash = find(l, 0, '-')) == l->n ||
        !number(l->t + dash + 1, l->n - dash - 1, 10, &steps))
        return false;
    if (!next(l) || l->t[0] != 'x' || (dot = find(l, 1, '.')) + 5 != l->n ||
        !number(l->t + 1, dot - 1, 10, &whole) || !number(l->t + dot + 1, 2, 10, &frac))
        return false;
    if (!next(l) || (at = find(l, 0, '@')) == l->n ||
        !number(l->t + at + 1, l->n - at - 1, 10, &offset))
        return false;
    uint32_t size = (whole * 100 + frac) / 25;   /* (code + 1) quarter dBs */
    if (size == 0 || size > 128 || steps > 0x7f || offset > 0x7f)
        return false;
    *out = (size - 1) << 16 | steps << 8 | offset;
    struct line peek = *l;
    if (next(&peek) && is(&peek, "+mute")) {
        *l = peek;
        *out |= 1u << 31;
    }
    return true;
}

/* "conn 0c* 0d ...": two hex digits each, `*` on the selected one. */
static void conns(struct line *l, struct widget *w)
{
    struct line peek = *l;
    uint32_t nid;
    while (next(&peek) && (peek.n == 2 || (peek.n == 3 && peek.t[2] == '*')) &&
           number(peek.t, 2, 16, &nid)) {
        if (w->nconn < MAX_CONN) {
            if (peek.n == 3) {
                w->conn_sel = w->nconn;
                w->has_sel = true;
            }
            w->conn[w->nconn++] = (uint16_t)nid;
        }
        *l = peek;
    }
}

/* "pwr D0/D0": actual / set. */
static bool power(struct line *l, uint8_t *out)
{
    if (!next(l) || l->n != 5 || l->t[0] != 'D' || l->t[2] != '/' || l->t[3] != 'D')
        return false;
    int act = digit(l->t[1], 10), set = digit(l->t[4], 10);
    if (act < 0 || set < 0)
        return false;
    *out = (uint8_t)(act << 4 | set);
    return true;
}

/* A widget's first line, from "caps" on. */
static bool widget_line(struct line *l, struct widget *w)
{
    if (!next(l) || !is(l, "caps") || !next_hex(l, 0, &w->caps))
        return false;
    bool ok = true;
    while (ok && next(l)) {
        if (is(l, "pwr"))
            ok = w->has_power = power(l, &w->power);
        else if (is(l, "out-amp"))
            ok = ampcaps(l, &w->amp_out);
        else if (is(l, "in-amp"))
            ok = ampcaps(l, &w->amp_in);
        else if (is(l, "conn"))
            conns(l, w);
    }
    return ok;
}

/* A pin's second line, from the configuration default on. */
static bool pin_line(struct line *l, struct widget *w)
{
    if (!next_hex(l, 1, &w->config))   /* "02214020:" */
        return false;
    bool ok = true;
    while (ok && next(l)) {
        if (is(l, "caps"))
            ok = next_hex(l, 0, &w->pincaps);
        else if (is(l, "ctl")) {
            uint32_t ctl = 0;
            ok = next_hex(l, 0, &ctl) && ctl <= 0xff;
            w->pin_ctl = (uint8_t)ctl;
        } else if (is(l, "eapd=on"))
            w->eapd = 2;
    }
    return ok;
}

/* "codec N: vvvv:dddd ..." and "codec N afg XX: widgets FF-LL ...". */
static void codec_line(struct line *l, struct codec *c)
{
    uint32_t v, afg, first, last;
    if (!next(l))
        return;
    if (l->t[l->n - 1] == ':') {
        if (next(l) && l->n == 9 && l->t[4] == ':' && number(l->t, 4, 16, &v) &&
            number(l->t + 5, 4, 16, &c->vendor))
            c->vendor |= v << 16;
        return;
    }
    if (!next(l) || !is(l, "afg") || !next_hex(l, 1, &afg) || !next(l) || !is(l, "widgets") ||
        !next(l) || l->n != 5 || !number(l->t, 2, 16, &first) || !number(l->t + 3, 2, 16, &last))
        return;
    c->afg = (uint8_t)afg;
    c->first = (uint8_t)first;
    c->count = (uint8_t)(last - first + 1);
    if (next(l) && is(l, "pwr"))
        (void)power(l, &c->afg_power);   /* stays 0 (D0) if it does not parse */
}

/* One line; false if it is a widget line that does not parse. */
static bool parse_line(struct line *l, struct codec *c)
{
    uint32_t cad, nid;
    if (!next(l))
        return true;
    if (is(l, "codec")) {
        codec_line(l, c);
        return true;
    }
    if (l->n < 2 || l->t[0] != 'c' || !number(l->t + 1, l->n - 1, 10, &cad) ||
        !next_hex(l, 0, &nid) || nid > 0x7f)
        return true;   /* not a widget line */
    c->cad = (uint8_t)cad;
    struct line peek = *l;
    if (next(&peek) && is(&peek, "cfg")) {
        struct widget *w = c->nw ? &c->w[c->nw - 1] : NULL;
        return w && w->nid == nid && pin_line(&peek, w);
    }
    if (c->nw >= MAX_WIDGETS)
        return false;
    struct widget *w = &c->w[c->nw++];
    w->nid = (uint8_t)nid;
    return next(l) && widget_line(l, w);   /* the type's name, then "caps" */
}

status_t hda_codec_from_dump(const char *text, size_t len, struct codec *out)
{
    *out = (struct codec){ .cad = 0 };
    const char *p = text, *end = text + len;
    while (p < end) {
        const char *e = p;
        while (e < end && *e != '\n')
            e++;
        struct line l = { .p = p, .end = e };
        if (!parse_line(&l, out))
            return ERR_INVALID_ARGS;
        p = e < end ? e + 1 : e;
    }
    return OK;
}

/* ---- the self-test ------------------------------------------------------------------ */

static struct widget *find_widget(struct codec *c, unsigned nid)
{
    for (unsigned i = 0; i < c->nw; i++)
        if (c->w[i].nid == nid)
            return &c->w[i];
    return NULL;
}

/* The rear line-out (pin 14, on DAC 02 through mixer 0c) has its output
 * on: the headphones must get a DAC of their own. */
static void rear_playing(struct codec *c)
{
    struct widget *w = find_widget(c, 0x14);
    if (w)
        w->pin_ctl = PINCTL_OUT;
}

/* The board describes no front headphone jack (pin 1b unconnected): the
 * first line-out, by association and sequence, is used instead. */
static void no_front_jack(struct codec *c)
{
    struct widget *w = find_widget(c, 0x1b);
    if (w)
        w->config = 0x411111f0;
}

/* QEMU's hda-output with mixer=off (tools/mixer-test.sh): the DAC has no
 * amplifier, so nothing on the path can mute it but the pin's output. */
static void no_amps(struct codec *c)
{
    struct widget *w = find_widget(c, 0x02);
    if (w) {
        w->caps &= ~(uint32_t)WCAP_OUT_AMP;
        w->amp_out = 0;
    }
}

/* The only output pin has no connection: no path. */
static void no_conn(struct codec *c)
{
    struct widget *w = find_widget(c, 0x03);
    if (w)
        w->nconn = 0;
}

static const struct fixture {
    const char *name;
    const char *text;
    size_t      len;
    void      (*change)(struct codec *c);   /* a variation on the parsed codec, or NULL */
    uint32_t    vendor;                     /* the codec's ids, as parsed */
    unsigned    rule;                       /* enum path_rule wanted */
    const char *path;                       /* hda_path_str wanted */
    const char *also;                       /* the path's `also` pins wanted */
    const char *gain;                       /* the output at its default gain (see gain_str) */
} fixtures[] = {
#define TEXT(t) t, sizeof(t) - 1
    { "hda-output", TEXT(qemu_output), NULL, 0x1af40012, PATH_LINE_OUT, "dac 02 -> pin 03", "",
      "vol 02 step 44 at -300 in -740..0; mutes" },
    { "hda-duplex", TEXT(qemu_duplex), NULL, 0x1af40022, PATH_LINE_OUT, "dac 02 -> pin 03", "",
      "vol 02 step 44 at -300 in -740..0; mutes" },
    { "hda-micro", TEXT(qemu_micro), NULL, 0x1af40032, PATH_SPEAKER, "dac 02 -> pin 03", "",
      "vol 02 step 44 at -300 in -740..0; mutes" },
    { "hda-output, no connection", TEXT(qemu_output), no_conn, 0x1af40012, PATH_NONE, "", "",
      "none" },
    { "hda-output, no amps", TEXT(qemu_output), no_amps, 0x1af40012, PATH_LINE_OUT,
      "dac 02 -> pin 03", "", "no steps; no mute" },
    { "alc897", TEXT(pc_alc897), NULL, 0x10ec0897, PATH_FRONT_HP,
      "dac 02 -> mixer 0c -> pin 1b", "14 18 19 1a", "vol 02 step 47 at -300 in -653..0; mutes" },
    { "alc897, rear line-out playing", TEXT(pc_alc897), rear_playing, 0x10ec0897, PATH_FRONT_HP,
      "dac 03 -> mixer 0d -> pin 1b", "15", "vol 03 step 47 at -300 in -653..0; mutes" },
    { "alc897, no front jack described", TEXT(pc_alc897), no_front_jack, 0x10ec0897,
      PATH_LINE_OUT, "dac 02 -> mixer 0c -> pin 14", "18 19 1a 1b",
      "vol 02 step 47 at -300 in -653..0; mutes" },
#undef TEXT
};

static bool same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void also_str(const struct path *p, char *buf, size_t size)
{
    size_t len = 0;
    buf[0] = 0;
    for (unsigned i = 0; i < p->nalso && len + 4 <= size; i++)
        len += (size_t)drv_snprintf(buf + len, size - len, "%s%02x", i ? " " : "", p->also[i]);
}

/* The output hda_output_init makes of path p (verbs.c): the volume amp's
 * node, its default step, that step's gain and its range in centibels:
 * "vol 02 step 47 at -300 in -653..0" ("none": no path; "no steps").
 * Then set_gain's clamping: far above the range must give its top, far
 * below its bottom ("clamps wrong" appended if not). Then whether an amp
 * on the path can mute it ("; mutes": the output stage stays on and the
 * amps are the mute; "; no mute": the pin's output is). Sends no verb:
 * the output is not open. */
static void gain_str(const struct codec *c, const struct path *p, char *buf, size_t size)
{
    struct output out;
    int32_t cb, lo, hi, top, bottom, x, y;
    hda_output_init(&out, p->rule == PATH_NONE ? NULL : c, p->rule == PATH_NONE ? NULL : p);
    status_t st = hda_output_gain(&out, &cb, &lo, &hi);
    const char *mute = out.mutes ? "; mutes" : "; no mute";
    if (st != OK) {
        drv_snprintf(buf, size, "%s%s", st == ERR_NOT_FOUND ? "none" : "no steps",
                     st == ERR_NOT_FOUND ? "" : mute);
        return;
    }
    unsigned step = out.step;
    (void)hda_output_set_gain(NULL, &out, 600);
    (void)hda_output_gain(&out, &top, &x, &y);
    (void)hda_output_set_gain(NULL, &out, -100000);
    (void)hda_output_gain(&out, &bottom, &x, &y);
    drv_snprintf(buf, size, "vol %02x step %u at %d in %d..%d%s%s", out.vol, step, cb, lo, hi,
                 top == hi && bottom == lo ? "" : " clamps wrong", mute);
}

/* One fixture; false (and a line into o) if it fails. */
static bool check(const struct fixture *f, struct codec *c, struct out *o)
{
    char got[64], also[40];
    struct path p;
    status_t st = hda_codec_from_dump(f->text, f->len, c);
    if (st != OK || c->vendor != f->vendor || !c->afg) {
        out_line(o, "path self-test: %s: the dump does not parse (%s, ids %08x, afg %02x)",
                 f->name, status_str(st), c->vendor, c->afg);
        return false;
    }
    if (f->change)
        f->change(c);
    st = hda_path_find(c, &p);
    hda_path_str(c, &p, got, sizeof(got));
    also_str(&p, also, sizeof(also));
    char gain[72];
    gain_str(c, &p, gain, sizeof(gain));
    if (p.rule == f->rule && (st == OK) == (f->rule != PATH_NONE) && same(got, f->path) &&
        same(also, f->also) && same(gain, f->gain))
        return true;
    out_line(o, "path self-test: %s: got \"%s\" (%s; also \"%s\"; %s), want \"%s\" (%s; also "
             "\"%s\"; %s)", f->name, got, hda_path_rule_name(p.rule), also, gain, f->path,
             hda_path_rule_name(f->rule), f->also, f->gain);
    return false;
}

status_t hda_fixture(const char *name, struct codec *out)
{
    for (unsigned i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); i++) {
        const struct fixture *f = &fixtures[i];
        if (!same(f->name, name))
            continue;
        status_t st = hda_codec_from_dump(f->text, f->len, out);
        if (st == OK && f->change)
            f->change(out);
        return st;
    }
    return ERR_NOT_FOUND;
}

bool hda_path_selftest(struct codec *scratch, struct out *o)
{
    unsigned n = sizeof(fixtures) / sizeof(fixtures[0]), passed = 0;
    for (unsigned i = 0; i < n; i++)
        passed += check(&fixtures[i], scratch, o);
    out_line(o, "path self-test: %u of %u fixture(s) passed", passed, n);
    return passed == n;
}

#define ROUNDTRIP_TEXT (32 * 1024)   /* one codec's dump: the ALC897's is ~7 KiB */

bool hda_path_roundtrip(const struct codec *c, const struct path *p, struct codec *scratch,
                        struct out *o)
{
    struct out text = { .buf = drv_malloc(ROUNDTRIP_TEXT), .cap = ROUNDTRIP_TEXT };
    if (!text.buf) {
        out_line(o, "codec %u: no memory to check the dump parses back", c->cad);
        return false;
    }
    hda_dump_codec(&text, c);
    struct path q;
    char a[64], b[64];
    status_t st = hda_codec_from_dump(text.buf, text.len, scratch);
    drv_free(text.buf);
    if (st == OK)
        (void)hda_path_find(scratch, &q);   /* compared below, found or not */
    if (st == OK) {
        hda_path_str(c, p, a, sizeof(a));
        hda_path_str(scratch, &q, b, sizeof(b));
    }
    if (st == OK && q.rule == p->rule && same(a, b))
        return true;
    out_line(o, "codec %u: its own dump parses back to another path (%s)", c->cad,
             st == OK ? b : status_str(st));
    return false;
}
