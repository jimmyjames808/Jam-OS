# The README's desktop pictures (tools/readme-shots.sh runs this): a plain
# boot at 2560x1440 with a usb-kbd on xhci port 2, a usb-mouse on port 3
# and a music stick on port 4 (/usb0/music, Artist/Album/Track.mp3, six
# albums: the second, Nocturnes, is played). The window geometry below
# follows from the tiler's and the floating defaults at that size (the
# room under the strip is x 6..2554, y 46..1434):
#   readme-tiled     terminal 1 (`ps`) on the left, Jamjar playing at the
#                    top right, terminal 2 under it
#   readme-search    the same desktop with the search box open (Super
#                    tapped alone), "ja" typed: the box is centred across
#                    the screen, x 1000..1560, its top at y 216
#   readme-floating  the same windows floating (Super+T), each where its
#                    tile was (its frame 4 pixels inside the tile), none
#                    overlapping
# A `monitor sendkey` holds its key 100 ms: the sleeps keep Super+Alt's
# pushes apart, so none is lost.
wait 120 Jam OS shell
wait {prompt}
seen 30 compositor: keyboard 0627:0001 ready
seen 30 report mouse ready
seen 60 init: /usb0 mounted
seen 60 music: ready
sleep 6
# ---- tiled: terminal 2 on the right; Jamjar splits it --------------------------------
monitor sendkey meta_l-ret
wait 10 init: terminal 2 opens
wait 30 Jam OS shell
wait 10 {prompt}
sleep 1
send uname -a
wait {prompt}
send ls /usb0/music
wait {prompt}
send jamjar &
wait 10 jamjar: in the background
sleep 8
# Jamjar has the keys: the albums, the second (Nocturnes), in order, play
type \t
type \e[B
type s
type \r
wait 30 music: playing /usb0/music/
sleep 1
# Jamjar above terminal 2, then its bottom edge down and its left edge left
monitor sendkey meta_l-shift-up
sleep 0.5
monitor sendkey meta_l-alt-down
sleep 0.3
monitor sendkey meta_l-alt-down
sleep 0.3
monitor sendkey meta_l-alt-down
sleep 0.3
monitor sendkey meta_l-alt-down
sleep 0.3
monitor sendkey meta_l-alt-down
sleep 0.3
monitor sendkey meta_l-alt-down
sleep 0.3
monitor sendkey meta_l-alt-left
sleep 0.3
monitor sendkey meta_l-alt-left
sleep 0.3
monitor sendkey meta_l-alt-left
sleep 0.3
monitor sendkey meta_l-alt-left
sleep 0.3
monitor sendkey meta_l-alt-left
sleep 0.3
monitor sendkey meta_l-alt-left
sleep 0.3
monitor sendkey meta_l-alt-left
sleep 0.3
monitor sendkey meta_l-alt-left
sleep 0.3
monitor sendkey meta_l-alt-left
sleep 1
# terminal 1 lists the programs (ps); the focus back to Jamjar
monitor sendkey meta_l-left
sleep 0.5
send ps
sleep 1
monitor sendkey meta_l-right
sleep 5
pointer 1500 700
sleep 1
shot readme-tiled
# ---- the search box over the same desktop -----------------------------------------
monitor sendkey meta_l
sleep 0.5
usbkeys ja
sleep 1.5
shot readme-search
monitor sendkey esc
sleep 1
# ---- floating: Super+T keeps the arrangement (owner, 2026-10-07): each
# window floats where its tile was, its frame 4 pixels inside the tile, so
# none overlaps; Jamjar's title bar is the top 27 pixels of its frame, from
# y 50 (its tile's top, 46, plus 4)
monitor sendkey meta_l-t
sleep 3
# the focus to Jamjar (a click on its title bar), the pointer resting there
pointer 1700 63
monitor mouse_button 1
sleep 0.2
monitor mouse_button 0
sleep 1.5
shot readme-floating
monitor quit
