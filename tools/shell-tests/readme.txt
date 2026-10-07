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
#   readme-floating  the same windows floating (Super+T), sized with
#                    Super+Alt (48 pixels a press) and dragged by their
#                    title bars apart, none overlapping: terminal 1 at
#                    (76,100) 958x1277, Jamjar at (1100,100) 1390x797,
#                    terminal 2 at (1100,964) 1390x413
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
# ---- floating: Jamjar in front at the cascade's end (573,366), terminal 1 at
# (513,306) behind it, terminal 2 at (543,336) at the back; each 1534x845.
# A title bar is 27 pixels: a window is dragged by a point 300 in and 13
# down its title bar, and raised by a click on a part of it that shows.
# Super+Alt+Left and Up shrink a floating window, Right and Down grow it.
monitor sendkey meta_l-t
sleep 3
# Jamjar (focused): 3 narrower, 1 shorter, then to (1100,100)
monitor sendkey meta_l-alt-left
sleep 0.3
monitor sendkey meta_l-alt-left
sleep 0.3
monitor sendkey meta_l-alt-left
sleep 0.3
monitor sendkey meta_l-alt-up
sleep 0.3
sleep 0.5
pointer 873 379
monitor mouse_button 1
sleep 0.2
pointer 1400 113
sleep 0.2
monitor mouse_button 0
sleep 1
# terminal 1: raised by its title bar left of Jamjar, to (76,100), then 12
# narrower and 9 taller
pointer 813 319
monitor mouse_button 1
sleep 0.2
pointer 376 113
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
monitor sendkey meta_l-alt-left
sleep 0.3
monitor sendkey meta_l-alt-left
sleep 0.3
monitor sendkey meta_l-alt-left
sleep 0.3
monitor sendkey meta_l-alt-left
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
monitor sendkey meta_l-alt-down
sleep 0.3
monitor sendkey meta_l-alt-down
sleep 0.3
monitor sendkey meta_l-alt-down
sleep 0.3
monitor sendkey meta_l-alt-down
sleep 0.3
sleep 0.5
# terminal 2: raised by the end of its title bar showing between terminal 1
# and Jamjar, 3 narrower and 9 shorter, then to (1100,964)
pointer 1067 349
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
monitor sendkey meta_l-alt-up
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
sleep 0.3
monitor sendkey meta_l-alt-up
sleep 0.3
monitor sendkey meta_l-alt-up
sleep 0.3
monitor sendkey meta_l-alt-up
sleep 0.3
sleep 0.5
pointer 843 349
monitor mouse_button 1
sleep 0.2
pointer 1400 977
sleep 0.2
monitor mouse_button 0
sleep 1
# the focus to Jamjar (a click on its title bar), the pointer on the
# wallpaper between the windows
pointer 1700 113
monitor mouse_button 1
sleep 0.2
monitor mouse_button 0
sleep 0.5
pointer 1067 1410
sleep 1.5
shot readme-floating
monitor quit
