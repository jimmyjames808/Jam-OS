#!/bin/sh
# The mouse, end to end: a plain boot ("shell") with a usb-kbd on xhci
# port 2 and a usb-mouse on port 3. tools/shell-tests/mouse.txt moves the
# mouse and presses its buttons through QEMU's monitor (mouse_move,
# mouse_button): usb-bus -> drv/hid -> console -> the key channel of the
# focused client. It checks
#   - the shell and tetris, which never asked for the mouse, are not
#     disturbed by it (no message they would misread as a key), and the
#     wheel still scrolls the console back;
#   - contest's client gets the movement, the buttons and the wheel;
#   - bin/mines, which asked, gets the pointer where the script put it
#     and every button edge: a whole game is played with clicks, each of
#     which mines says on the console (`trace`), so the serial log shows
#     them; the arrow is in the screenshots (<outdir>/mouse-*.png).
# QEMU's usb-mouse has a wheel in its report descriptor, so hid drives it
# in the report protocol ("report mouse ready" in the log); with
# MOUSE_HIDBOOT=1 the boot word hidboot keeps it in the boot protocol
# ("boot mouse ready"), whose fourth byte QEMU fills with the wheel too.
# The clicks are at pixel positions worked out for OVMF's 1280x800, so
# this script has no 2560x1440 variant (tools/apps-test.sh has one).
# QEMU_SMP passes through. Usage: tools/mouse-test.sh <outdir> [name];
# exit 0 on PASS.
set -eu
out=$1 name=${2:-mouse}
mkdir -p "$out"
if [ "${MOUSE_HIDBOOT:-0}" = 1 ]; then
    words="shell hidboot" ready="boot mouse ready"
else
    words="shell" ready="report mouse ready"
fi
# shellcheck disable=SC2086 # $words is the command line, word by word
if QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_INPUT=tools/shell-tests/mouse.txt \
   QEMU_USB="-device usb-kbd,bus=xhci.0,port=2 -device usb-mouse,bus=xhci.0,port=3" \
   tools/qemu-test.sh "$out" "$name" $words; then
    if grep -aq "hid 0627:0001 if 0: $ready" "$out/$name.log"; then
        echo "$name: PASS"
        exit 0
    fi
    echo "$name: no '$ready' in the log"
fi
echo "$name: FAIL (see $out/$name.log)"
exit 1
