#!/bin/sh
# The boot splash (user/apps/splash, docs/history/AS-PLAN.md) in QEMU, with
# QEMU_SPLASH=1 (tools/qemu-test.sh leaves the splash out otherwise) and an
# intel-hda + hda-output codec writing what it plays to a WAV file (as
# tools/mixer-test.sh). Six boots, all the compositor's (M12's Q14: the
# `nocomp` runs, the console on the whole screen, are gone):
#   comp     a plain boot (splash-comp.txt): twelve screenshots 0.1 s apart
#            from init's start are each all #1E1A1D or a frame of the
#            video (the compositor's `splash` argument: only the splash's
#            background until its window maps, never the desktop first);
#            two screenshots a second apart are frames of the video, in
#            order (its full-screen window, over the first terminal's; at
#            OVMF's 1280x800 the video at 1x, centred), around it the
#            background; the capture is the video's sound from where the
#            splash said it joined, to the end (30 dB above the
#            difference: tools/splash-check.py); the order in the log
#            (first frame, sound, played, the shell up); the sound waited
#            for and started with the picture (joins at 0 ms); the last
#            frame lingers 0.5 s after the end; a line typed while it
#            plays, one once it has played and one right after its window
#            has gone all reach the shell (the splash's window takes no
#            keys: the terminal's keeps them), and no key skips it;
#            `run splash --selftest` passes; `run splash --alpha` draws the
#            logo with libfun's anti-aliased discs (their colours, a partly
#            covered edge) in a full-screen window. With QEMU's network
#            (QEMU_NET=1, untagged): netstack's Connected notice, posted
#            while the splash plays, is held and shown only once the splash
#            is over (the compositor's "held notice" line after the
#            splash's "played at"; skipped, said, if the lease comes after
#            the splash).
#   keys     at the PC's 2560x1440 (QEMU's VGA; the video at 1:1): a
#            screenshot is a frame at 1:1; lines typed while it plays don't
#            skip it (it plays to the end, the mixer takes its whole sound)
#            and reach the shell once it is up, which runs them.
#   verbose, nosplash   the boot words: the kernel's text log is on the
#            screen as init starts, no splash runs, the shell comes up.
#   panic    `shell testpf`: a panic at boot with the screen quiet draws
#            the calm panic screen over it (one before the clock is
#            measured: its details at once, and no reset).
#   hang     the test word `splashhang` (splash-hang.txt): a splash that
#            never finishes; init starts the shell anyway, 20 s after the
#            splash's start (not much later, never sooner), and the shell
#            runs what is typed. Booted with every option word the kernel
#            hands init at once: init's first line lists all of them.
# It prints the time from the kernel's start to the first frame and to the
# shell, with the splash and without (the nosplash boot).
# QEMU_SMP passes through. Usage: tools/splash-test.sh <outdir>; exit 0 on PASS.
set -eu
out=$1
mkdir -p "$out"
ok=1
snd() {
    echo "-audiodev wav,id=snd0,path=$1,out.frequency=48000,out.channels=2,out.format=s16 \
-device intel-hda,id=hda0 -device hda-output,bus=hda0.0,cad=0,audiodev=snd0,mixer=off"
}
# The uptime (s) of the first log line with this text.
at() {
    grep -aF -- "$2" "$1" | head -1 | sed -n 's/^\[ *\([0-9.]*\)\].*/\1/p'
}
need() {   # log, text...
    log=$1
    shift
    for want in "$@"; do
        grep -aqF -- "$want" "$log" || { echo "splash: no line with \"$want\" in $log"; ok=0; }
    done
}
check() {
    python3 tools/splash-check.py "$@" || ok=0
}

# ---- comp: the compositor's plain boot -------------------------------------------
rm -f "$out/comp.wav"
QEMU_SPLASH=1 QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_EXTRA="$(snd "$out/comp.wav")" \
    QEMU_NET=1 QEMU_NET_VLAN=none \
    QEMU_INPUT=tools/shell-tests/splash-comp.txt tools/qemu-test.sh "$out" comp shell \
    > "$out/comp.out" 2>&1 || { echo "comp: the script failed"; tail -3 "$out/comp.out"; ok=0; }
log=$out/comp.log
need "$log" "init: the compositor draws the screen" "splash: played at" \
    "the background only until the splash shows" "args: init shell splash" \
    "at 1/2 (box) on 1280x800" "they start together" "splash: sound joins at 0 ms" \
    "init: the shell is up" "init: the splash has played: starting the shell" \
    "typed-kept" "right-after" "bin/splash exited with code 0" "splash: selftest PASSED"
t_note=$(at "$log" ": Connected: ")
t_played=$(at "$log" "splash: played at")
t_held=$(at "$log" "held notice")
if [ -z "$t_note" ] || [ -z "$t_played" ] ||
   python3 -c "import sys; sys.exit(float(sys.argv[1]) < float(sys.argv[2]))" "$t_note" "$t_played"; then
    echo "comp: no Connected notice during the splash (${t_note:-none}): its hold not checked"
elif [ -z "$t_held" ] ||
     ! python3 -c "import sys; sys.exit(float(sys.argv[1]) < float(sys.argv[2]))" "$t_held" "$t_played"; then
    echo "comp: the Connected notice ($t_note s) wasn't held until the splash was over" \
         "(played at $t_played s, held notices shown at ${t_held:-never})"
    ok=0
else
    echo "comp: the Connected notice at $t_note s was held, shown at $t_held s (the splash played at $t_played s)"
fi
if grep -aqF "skipped by a key" "$log"; then echo "comp: a key skipped the splash"; ok=0; fi
t_first=$(at "$log" "splash: first frame") t_shell=$(at "$log" "init: the shell is up")
python3 -c "import sys; a = [float(x) for x in sys.argv[1:]]; sys.exit(a != sorted(a))" \
    "$t_first" "$t_played" "$t_shell" ||
    { echo "comp: out of order: first frame $t_first, played $t_played, shell $t_shell"; ok=0; }
grep -aE "splash: (first frame|waited|sound joins|the sound is heard|played at|the shell is up)" \
    "$log" | sed 's/^/  /'
# The logo lingers: the window goes LINGER (0.5 s) after the end at the
# earliest, however soon the shell is up.
back=$(grep -aE "giving the screen back [0-9]+ ms after it" "$log" | head -1 |
       sed -n 's/.*giving the screen back \([0-9]*\) ms after it.*/\1/p')
[ -n "$back" ] && [ "$back" -ge 450 ] || { echo "comp: no linger (${back:-?} ms)"; ok=0; }
join=$(grep -aE "splash: sound joins at [0-9]+ ms" "$log" | head -1 |
       sed -n 's/.*joins at \([0-9]*\) ms.*/\1/p')
check early "$out"/comp-early-*.png
check frames "$out/comp-a.png" "$out/comp-b.png"
check alpha "$out/splash-alpha.png"
need "$log" "splash: alpha demo drawn"
if [ -n "$join" ]; then check sound "$out/comp.wav" "$join"; else ok=0; fi

# ---- keys: typed while it plays, at 2560x1440 ----------------------------------
rm -f "$out/keys.wav"
QEMU_SPLASH=1 QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} \
    QEMU_EXTRA="$(snd "$out/keys.wav") -vga none -device VGA,xres=2560,yres=1440,vgamem_mb=64" \
    QEMU_INPUT=tools/shell-tests/splash-keys.txt tools/qemu-test.sh "$out" keys shell \
    > "$out/keys.out" 2>&1 || { echo "keys: the script failed"; tail -3 "$out/keys.out"; ok=0; }
need "$out/keys.log" "at 1:1 on 2560x1440" "splash: played at" "typed-kept" \
    "bin/splash exited with code 0"
if grep -aqF "skipped by a key" "$out/keys.log"; then echo "keys: a key skipped the splash"; ok=0; fi
check frame "$out/splash-hd.png" 2560
# The whole sound: the mixer took (nearly) all of the track (the capture
# itself isn't checked here: at 2560x1440 QEMU's emulation is too busy to
# keep its own audio timing).
taken=$(grep -aE "\(splash\) closed .*: [0-9]+ frames taken" "$out/keys.log" | head -1 |
        sed -n 's/.*: \([0-9]*\) frames taken.*/\1/p')
long=$(grep -aE "splash: first frame .* [0-9]+ ms long" "$out/keys.log" | head -1 |
       sed -n 's/.* \([0-9]*\) ms long.*/\1/p')
whole=$(( ${long:-0} * 48 ))
echo "keys: the mixer took ${taken:-?} frames of the splash's sound (the whole track: $whole)"
[ -n "$taken" ] && [ "$taken" -ge $((whole * 9 / 10)) ] ||
    { echo "keys: the sound was cut short"; ok=0; }

# ---- verbose and nosplash: the text log --------------------------------------
for word in verbose nosplash; do
    QEMU_SPLASH=1 QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_INPUT=tools/shell-tests/splash-off.txt \
        tools/qemu-test.sh "$out" "$word" shell "$word" > "$out/$word.out" 2>&1 ||
        { echo "$word: the script failed"; tail -3 "$out/$word.out"; ok=0; }
    need "$out/$word.log" "init: hello from ring 3" "init: the shell is up"
    if grep -aq "args: init shell splash" "$out/$word.log"; then
        echo "$word: init was told to play the splash"
        ok=0
    fi
    if grep -aq "\[splash\]" "$out/$word.log"; then echo "$word: the splash ran"; ok=0; fi
    mv "$out/splash-off.png" "$out/$word-text.png" 2>/dev/null || true
    check text "$out/$word-text.png"
done
t_plain=$(at "$out/nosplash.log" "init: the shell is up")

# ---- panic: drawn over the quiet screen -----------------------------------------
QEMU_SPLASH=1 QEMU_TIMEOUT=120 tools/qemu-test.sh "$out" panic shell testpf \
    > "$out/panic.out" 2>&1 || true
need "$out/panic.log" "KERNEL PANIC"
check calm "$out/panic.png" details

# ---- hang: a splash that never finishes ---------------------------------------
# Booted with every option word the kernel passes init at once (nine with
# bootdisk=, which Limine gives): all of them reach it, none dropped.
QEMU_SPLASH=1 QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_INPUT=tools/shell-tests/splash-hang.txt \
    tools/qemu-test.sh "$out" hang shell splashhang hidboot net vlan=none nospare vtdtest \
    > "$out/hang.out" 2>&1 || { echo "hang: the script failed"; tail -3 "$out/hang.out"; ok=0; }
words="init shell splash hidboot net vlan=none bootdisk=0x[0-9a-f]+ splashhang nospare vtdtest"
grep -aqE "init: hello from ring 3 \(10 args: $words\)" "$out/hang.log" ||
    { echo "hang: init was not given all its words"; grep -a "hello from ring 3" "$out/hang.log"; ok=0; }
need "$out/hang.log" "splash: hanging, as asked" "the splash didn't finish in 20 s: starting the shell" \
    "init: the shell is up" "hang-ok"
t_hang=$(at "$out/hang.log" "splash: hanging") t_gave=$(at "$out/hang.log" "didn't finish in")
python3 -c "import sys; a, b = float(sys.argv[1]), float(sys.argv[2]); sys.exit(not 17 < b - a < 23)" \
    "${t_hang:-0}" "${t_gave:-0}" ||
    { echo "hang: the shell waited ${t_hang:-?} -> ${t_gave:-?} s, want about 20 s"; ok=0; }

echo "splash: from the kernel's start: first frame ${t_first:-?} s, shell up ${t_shell:-?} s" \
     "with the splash, ${t_plain:-?} s without (nosplash)"
if [ $ok = 1 ]; then
    echo "splash: PASS"
    exit 0
fi
echo "splash: FAIL"
exit 1
