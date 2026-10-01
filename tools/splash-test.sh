#!/bin/sh
# The boot splash (user/apps/splash, docs/AS-PLAN.md) in QEMU, with
# QEMU_SPLASH=1 (tools/qemu-test.sh leaves the splash out otherwise) and an
# intel-hda + hda-output codec writing what it plays to a WAV file (as
# tools/mixer-test.sh). Five boots:
#   splash   a plain boot (OVMF's 1280x800: the video at 1x, centred), the
#            serial script tools/shell-tests/splash.txt. Checked: the
#            screen is all #1E1A1D when init starts (the kernel's quiet
#            fbcon, no text); two screenshots a second apart are frames of
#            the video, in order, around it the background; after the
#            hand-back the console's text; the capture is the video's sound
#            from where the splash said it joined, to the end (30 dB above
#            the difference: tools/splash-check.py); the order in the log
#            (first frame, sound, played, the shell up, the screen back);
#            the sound waited for and started with the picture (joins at
#            0 ms); the last frame lingers 0.5 s after the end;
#            `run splash --selftest` passes; `run splash --alpha` draws the
#            logo with libfun's anti-aliased discs (their colours, a partly
#            covered edge).
#   keys     at the PC's 2560x1440 (QEMU's VGA; the video at 1:1): a
#            screenshot is a frame at 1:1; lines typed while it plays don't
#            skip it (it plays to the end, the mixer takes its whole sound)
#            and reach the shell once it is up, which runs them.
#   verbose, nosplash   the boot words: the kernel's text log is on the
#            screen as init starts, no splash runs, the shell comes up.
#   panic    `shell testpf`: a panic at boot with the screen quiet draws
#            the red panic screen over it.
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

# ---- splash: a plain boot ----------------------------------------------------
rm -f "$out/splash.wav"
QEMU_SPLASH=1 QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_EXTRA="$(snd "$out/splash.wav")" \
    QEMU_INPUT=tools/shell-tests/splash.txt tools/qemu-test.sh "$out" splash shell \
    > "$out/splash.out" 2>&1 || { echo "splash: the script failed"; tail -3 "$out/splash.out"; ok=0; }
log=$out/splash.log
need "$log" "quiet for the boot splash" "args: init shell splash" \
    "at 1/2 (box) on 1280x800" "they start together" "splash: sound joins at 0 ms" \
    "the shell is up" "splash: played at" "init: the splash has played: starting the shell" \
    "giving the screen back" "console: the screen is back" \
    "bin/splash exited with code 0" "splash: selftest PASSED"
t_first=$(at "$log" "splash: first frame") t_shell=$(at "$log" "init: the shell is up")
t_played=$(at "$log" "splash: played at") t_back=$(at "$log" "console: the screen is back")
python3 -c "import sys; a = [float(x) for x in sys.argv[1:]]; sys.exit(a != sorted(a))" \
    "$t_first" "$t_played" "$t_shell" "$t_back" ||
    { echo "splash: out of order: first frame $t_first, played $t_played, shell $t_shell," \
           "screen back $t_back"; ok=0; }
grep -aE "splash: (first frame|waited|sound joins|the sound is heard|played at|the shell is up)" \
    "$log" | sed 's/^/  /'
# The logo lingers: the screen goes back LINGER (0.5 s) after the end at the
# earliest, however soon the shell is up.
back=$(grep -aE "giving the screen back [0-9]+ ms after it" "$log" | head -1 |
       sed -n 's/.*giving the screen back \([0-9]*\) ms after it.*/\1/p')
[ -n "$back" ] && [ "$back" -ge 450 ] || { echo "splash: no linger (${back:-?} ms)"; ok=0; }
join=$(grep -aE "splash: sound joins at [0-9]+ ms" "$log" | head -1 |
       sed -n 's/.*joins at \([0-9]*\) ms.*/\1/p')
check quiet "$out/splash-quiet.png"
check frames "$out/splash-a.png" "$out/splash-b.png"
check text "$out/splash-shell.png"
check alpha "$out/splash-alpha.png"
need "$log" "splash: alpha demo drawn"
if [ -n "$join" ]; then check sound "$out/splash.wav" "$join"; else ok=0; fi

# ---- keys: typed while it plays, at 2560x1440 ----------------------------------
rm -f "$out/keys.wav"
QEMU_SPLASH=1 QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} \
    QEMU_EXTRA="$(snd "$out/keys.wav") -vga none -device VGA,xres=2560,yres=1440,vgamem_mb=64" \
    QEMU_INPUT=tools/shell-tests/splash-keys.txt tools/qemu-test.sh "$out" keys shell \
    > "$out/keys.out" 2>&1 || { echo "keys: the script failed"; tail -3 "$out/keys.out"; ok=0; }
need "$out/keys.log" "at 1:1 on 2560x1440" "splash: played at" "typed-kept" \
    "console: the screen is back"
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
check red "$out/panic.png"

echo "splash: from the kernel's start: first frame ${t_first:-?} s, shell up ${t_shell:-?} s" \
     "with the splash, ${t_plain:-?} s without (nosplash)"
if [ $ok = 1 ]; then
    echo "splash: PASS"
    exit 0
fi
echo "splash: FAIL"
exit 1
