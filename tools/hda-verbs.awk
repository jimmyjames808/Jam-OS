# Classify the verbs QEMU's HD Audio codecs got, from their debug=3 lines
# on QEMU's stderr ("hda_audio_command: nid N ... verb 0xVVV, payload
# 0xPPPP"; QEMU prints the 4-bit verbs as 0x200, 0x300, 0xa00, 0xb00 with
# the 16-bit payload). Used by tools/hda-test.sh, hda-stream-test.sh and
# beep-test.sh. One line out per verb, in order:
#   get     a GET verb (0xf00-0xfff, 0xa00, 0xb00): reads, changes nothing
#   silent  a SET that can't make sound: power D0 (0x705 payload 0), a
#           connection select (0x701), pin control with the output and
#           headphone bits clear (0x707), an amp SET with the mute bit
#           (0x300, bit 7), EAPD off (0x70c, bit 1 clear)
#   conv    the converter's stream format (0x200) or stream tag (0x706)
#   jack    the jacks' verbs (drivers/hda/jack.c): unsolicited response
#           enable (0x708) and the pin sense trigger (0x709); neither can
#           make sound, but they are counted apart so a test can say which
#           nodes got them
#   power   the output stage on: pin control with the output or headphone
#           bit, EAPD on, on a codec whose path amps can mute it (run with
#           -v stage=1: QEMU's codecs with their mixer, the PC's). The
#           driver turns it on once, with every amp on the path muted, and
#           leaves it on (drivers/hda/verbs.c, hda_output_stage): on its
#           own it lets nothing out
#   open    a SET that lets sound out: an amp SET without the mute bit;
#           without -v stage=1 (QEMU's codec with mixer=off, whose amp
#           can't mute: the pin is the only mute, switched with each
#           stream) the power verbs too
#   bad     anything else (the allow-list should make this impossible)
# followed by the node and the verb and payload in hex. Then the codec's
# state as the verbs left it, by the SETs alone (each amp by node, side
# out/in and index; each pin control; each EAPD), in three lines:
#   "power on N off F first-tag T left P": the power verbs, the times a
#   pin or EAPD went off again, the power verbs sent before the first
#   stream tag (the stage on before any stream), and how many pins and
#   EAPDs are on at the end;
#   "total get G silent S conv C open O bad B"
#   "state untagged U released R left L", where U counts the open verbs
#   sent while no converter had a stream tag (0x706 with a non-zero tag),
#   R the times a converter's tag went back to 0 while something was
#   open, and L what is still open at the end: all three must be 0 for
#   "unmuted only while a stream runs, and muted again after".
# With several codecs in one trace the nodes are not told apart: use it
# on one codec's trace.
function hex(s,   i, c, v) {
    v = 0
    for (i = 3; i <= length(s); i++) {
        c = index("0123456789abcdef", tolower(substr(s, i, 1)))
        if (c == 0) break
        v = v * 16 + c - 1
    }
    return v
}
/hda_audio_command: nid/ {
    nid = -1; v = -1; p = 0
    for (i = 1; i <= NF; i++) {
        if ($i == "nid") nid = $(i + 1) + 0
        if ($i == "verb") v = hex($(i + 1))
        if ($i == "payload") p = hex($(i + 1))
    }
    if (v >= 3840 || v == 2560 || v == 2816)                    # 0xf00-0xfff, 0xa00, 0xb00
        c = "get"
    else if ((v == 1797 && p == 0) || v == 1793)                # 0x705 D0, 0x701
        c = "silent"
    else if (v == 1799)                                         # 0x707
        c = int(p / 64) % 4 == 0 ? "silent" : "power"
    else if (v == 768)                                          # 0x300
        c = int(p / 128) % 2 == 1 ? "silent" : "open"
    else if (v == 1804)                                         # 0x70c
        c = int(p / 2) % 2 == 0 ? "silent" : "power"
    else if (v == 512 || v == 1798)                             # 0x200, 0x706
        c = "conv"
    else if (v == 1800 || v == 1801)                            # 0x708, 0x709
        c = "jack"
    else
        c = "bad"
    # the state the SETs leave
    k = ""
    if (v == 768)
        k = nid (p >= 32768 ? ":out" : ":in" int(p / 256) % 16)
    else if (v == 1799)
        k = nid ":ctl"
    else if (v == 1804)
        k = nid ":eapd"
    if (k != "" && v != 768) {
        # a pin control or EAPD: the output stage
        on = (c == "power")
        if (on && !ever_tagged)
            early++
        if (!on && pw[k] == 1)
            off++
        if (on != (pw[k] == 1))
            npow += on ? 1 : -1
        pw[k] = on
        if (on)
            pon++
        # no amp can mute: the pin is the mute, so the stage is open
        if (on && !stage)
            c = "open"
    }
    if (k != "") {
        now = (c == "open")
        if (now && !tagged)
            untagged++
        if (now != (st[k] == 1))
            nopen += now ? 1 : -1
        st[k] = now
    }
    if (v == 1798) {
        tag[nid] = int(p / 16) % 16
        tagged = 0
        for (t in tag)
            if (tag[t])
                tagged = 1
        if (tagged)
            ever_tagged = 1
        if (!tag[nid] && nopen > 0)
            released++
    }
    n[c]++
    printf "%s nid %d verb %#x payload %#x\n", c, nid, v, p
}
END {
    printf "power on %d off %d first-tag %d left %d\n", pon, off, early, npow
    printf "total get %d silent %d conv %d open %d bad %d jack %d\n",
        n["get"], n["silent"], n["conv"], n["open"], n["bad"], n["jack"]
    printf "state untagged %d released %d left %d\n", untagged, released, nopen
}
