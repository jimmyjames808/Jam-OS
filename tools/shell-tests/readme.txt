# The README's desktop pictures (tools/readme-shots.sh runs this): a plain
# boot at 2560x1440 with a usb-kbd on xhci port 2, a usb-mouse on port 3
# and a music stick on port 4 (/usb0/music, Artist/Album/Track.mp3, six
# albums: the second, Nocturnes, is played). The window geometry below
# follows from the tiler's and the floating defaults at that size (the
# room under the strip is x 6..2554, y 46..1434):
#   readme-tiled     terminal 1 (`ps`) on the left, Jamjar playing at the
#                    top right, terminal 2 under it
#   readme-floating  the same windows floating (Super+T), moved apart:
#                    Jamjar in front, the pointer on its circles, and a
#                    notice with buttons (`notify`) in the top right
#   readme-search    the search box (Super tapped alone), "ja" typed
#   readme-popover   the volume popover, the notice answered first
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
# a notice with buttons (it stays until one is pressed), posted from
# terminal 2 (under Jamjar), then the focus back to Jamjar: the focused
# window ends up in front when the screen floats
monitor sendkey meta_l-down
sleep 0.5
send notify -b Yes -b No Tea? The kettle is on
wait 10 posted
sleep 0.5
monitor sendkey meta_l-up
sleep 0.5
# ---- floating: Jamjar in front at the cascade's end (573,366), terminal 1 at
# (513,306) behind it, terminal 2 at (543,336) at the back; each 1534 wide.
# A title bar is 27 pixels: each window is dragged by a point 300 in and
# 13 down its title bar.
monitor sendkey meta_l-t
sleep 3
# Jamjar to (150,200)
pointer 873 379
monitor mouse_button 1
sleep 0.2
pointer 450 213
sleep 0.2
monitor mouse_button 0
sleep 1
# terminal 2: raised by a click on the end of its title bar that shows,
# made smaller, then to (1380,790)
pointer 2062 349
monitor mouse_button 1
sleep 0.2
monitor mouse_button 0
sleep 0.5
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
monitor sendkey meta_l-alt-up
sleep 0.3
monitor sendkey meta_l-alt-up
sleep 0.3
monitor sendkey meta_l-alt-up
sleep 0.3
monitor sendkey meta_l-alt-up
sleep 0.3
monitor sendkey meta_l-alt-up
sleep 0.5
pointer 843 349
monitor mouse_button 1
sleep 0.2
pointer 1680 803
sleep 0.2
monitor mouse_button 0
sleep 1
# terminal 1: raised the same way, made smaller, then to (1300,110)
pointer 1850 319
monitor mouse_button 1
sleep 0.2
monitor mouse_button 0
sleep 0.5
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
monitor sendkey meta_l-alt-up
sleep 0.3
monitor sendkey meta_l-alt-up
sleep 0.3
monitor sendkey meta_l-alt-up
sleep 0.3
monitor sendkey meta_l-alt-up
sleep 0.3
monitor sendkey meta_l-alt-up
sleep 0.5
pointer 813 319
monitor mouse_button 1
sleep 0.2
pointer 1600 123
sleep 0.2
monitor mouse_button 0
sleep 1
# Jamjar raised again by its title bar; the pointer on its circles
pointer 700 213
monitor mouse_button 1
sleep 0.2
monitor mouse_button 0
sleep 0.5
pointer 188 219
sleep 1.5
shot readme-floating
# ---- the search box ----------------------------------------------------------------
monitor sendkey meta_l
sleep 0.5
usbkeys ja
sleep 1
pointer 1150 292
sleep 1
shot readme-search
monitor sendkey esc
sleep 1
# ---- the volume popover: the notice answered first (its Yes, 58 pixels into
# the card, which is 300 wide and 10 from the right edge), then the icon
pointer 2308 112
monitor mouse_button 1
sleep 0.2
monitor mouse_button 0
sleep 1
pointer 2415 20
monitor mouse_button 1
sleep 0.2
monitor mouse_button 0
sleep 1.5
pointer 2350 240
sleep 1
shot readme-popover
monitor quit
