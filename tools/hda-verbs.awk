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
#   open    a SET that lets sound out: an amp SET without the mute bit,
#           pin control with the output or headphone bit, EAPD on
#   bad     anything else (the allow-list should make this impossible)
# followed by the node and the verb and payload in hex. The last line:
# "total get G silent S conv C open O bad B".
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
        c = int(p / 64) % 4 == 0 ? "silent" : "open"
    else if (v == 768)                                          # 0x300
        c = int(p / 128) % 2 == 1 ? "silent" : "open"
    else if (v == 1804)                                         # 0x70c
        c = int(p / 2) % 2 == 0 ? "silent" : "open"
    else if (v == 512 || v == 1798)                             # 0x200, 0x706
        c = "conv"
    else
        c = "bad"
    n[c]++
    printf "%s nid %d verb %#x payload %#x\n", c, nid, v, p
}
END {
    printf "total get %d silent %d conv %d open %d bad %d\n",
        n["get"], n["silent"], n["conv"], n["open"], n["bad"]
}
