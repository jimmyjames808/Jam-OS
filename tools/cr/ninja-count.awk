# Counts what `ninja -n` says it would do: its lines "[12/345] CXX obj/x.o"
# by the kind in the description, one "key count" line each (keys steps,
# compile, link, stamp, other; always all five). Used by the port's `plan`
# (ports/chromium/build.sh) and tools/cr/build.sh's `size` (the claim for a
# count, tools/cr/build-test.sh feeds it a faked output). Chromium's GN rules
# describe a step by its kind first: CXX, CC, ASM, OBJC (compiles), RUST
# (rustc), LINK, SOLINK, SOLINK_MODULE (links), STAMP (free: a touch), and AR,
# ACTION, COPY, ... (other).
/^\[[0-9]+\/[0-9]+\] / {
    k = $2
    n++
    if (k ~ /^(CXX|CC|ASM|OBJC|OBJCXX|RUST|RUSTC)$/) {
        c++
    } else if (k ~ /^(LINK|SOLINK|SOLINK_MODULE)$/) {
        l++
    } else if (k == "STAMP") {
        s++
    } else {
        o++
    }
}
END {
    printf "steps %d\ncompile %d\nlink %d\nstamp %d\nother %d\n", n, c, l, s, o
}
