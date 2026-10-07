# The network

Jam OS has its own driver for the PC's Realtek RTL8125B (and for QEMU's
e1000e, for the tests), a network stack in a process of its own (lwIP:
IPv4, ARP, ICMP, UDP and TCP), and every frame in one network mode and
nothing else ever sent. On top of it: the address from DHCP or the
settings, names from DNS, `ping` and `host`, UDP and TCP sockets for
programs (their list asks for `svc net`, and `svc dns` for names; taking
connections needs `svc net listen`), the clock from the network (SNTP),
the boot log sent to the Mac as it is written, files over plain HTTP both
ways (`fetch` from a web server, `serve` a file in the background), a
throughput tester (`speed`, with `tools/speed.py` on the Mac), and
`update`, which runs the Mac's newest build, signed with the owner's key,
without moving the stick. The design is in
[ARCHITECTURE.md](../ARCHITECTURE.md#networking); the plans and what each
stage built are in [M9-PLAN.md](M9-PLAN.md) and
[M9.5-PLAN.md](M9.5-PLAN.md); the status is in
[ROADMAP.md](ROADMAP.md) (built and tested in QEMU; pings, names, the
log to the Mac and `update` have also run on the PC; the PC sign-off is
still to come).

## The network's mode

Jam OS sends in one network mode only, and nothing else ever leaves
([ARCHITECTURE.md](../ARCHITECTURE.md#networking)):

- **untagged** (the default of a build of this repository): plain
  Ethernet, as on an ordinary home or office network. No frame is ever
  sent with a VLAN tag, and tagged frames that arrive are dropped;
- **a VLAN** (1..4094): every frame is tagged 802.1Q with it, and only
  frames tagged with it are taken in. For a switch port that carries the
  VLAN tagged. The owner's PC uses VLAN 21
  ([HARDWARE.md](HARDWARE.md#the-network)).

### Building for a VLAN

To build for a VLAN, copy `local.mk.example` to `local.mk` (git ignores
it) and set `JAMOS_VLAN := 21` (your VLAN), or give it once:
`make JAMOS_VLAN=21`. Every `make` says which default it built
(`network default: VLAN 21 (local.mk)`, `network default: untagged (no
local.mk)`), and the boot image's `build.txt` records it (`net vlan21`,
`net untagged`). The boot word
`vlan=<id>`, `vlan=none` or `vlan=off` (the network card left off
altogether) overrides it for one boot, and a `reboot` keeps it. `update`
refuses a build whose default differs from the running one's unless given
`-f`, and `make flash` asks before it writes an untagged build. On the PC
the everyday entry "Jam OS" is on the network; "Developer > Jam OS (no network)"
(`vlan=off`) leaves the network card alone. In QEMU, `tools/qemu-test.sh` with
`QEMU_NET=1` gives the machine a card and a small network of its own
([TESTING.md](TESTING.md#the-network-peer)).

## Commands

| Command | What it does |
|---|---|
| `net` | the address, gateway and DNS servers, the link (speed, VLAN or untagged) and the main counts |
| `net stats` | every count netstack keeps, and the network card's own |
| `ping <address or name> [-c count] [-s size]` | ICMP echo, one a second; Ctrl+C stops it |
| `host <name>` | the name's IPv4 addresses, from the DNS server |
| `update [-n \| -m \| -w] [-r] [-f] [address]` | fetch the build the Mac serves, have init check its signature and files, write it (and its boot menu) to the stick and load it, then stop: `reboot` starts it, `reboot -f` too (from the stick); `-m` loads it into memory only, the stick untouched; `-n` fetches and checks only; `-w` is the same as plain `update`; `-r` reboots into it at once; `-f` takes a build whose network default isn't this one's |
| `fetch <url> [file \| -]` | download a file over plain HTTP (`http://` only: https needs TLS, which Jam OS doesn't have yet), into the URL's last name here, a file or a folder, or a pipe |
| `serve [<file> [port] \| stop [port]]` | serve one file over HTTP in the background (port 8080 unless given); alone, what is served; `stop`, stop it |
| `speed <host> [port] [-r \| -u] [-t s]`, `speed -l [port]` | network throughput against `tools/speed.py` on the Mac, either way, TCP or UDP |

## Settings

The settings, in `/data/etc/settings` on the stick (edit `etc/settings`
on the Mac, or in Jam OS), for example:

```
net.address = 10.2.21.240/24 10.2.21.1 10.2.21.1
net.host = 10.2.21.174
```

| Key | What |
|---|---|
| `net.address` | a static address: `<address>/<prefix> [<gateway> [<dns> [<dns>]]]`. Without it the DHCP client gets one |
| `net.host` | the Mac's address (10.2.21.174 for the owner's, on VLAN 21): where the log goes and where `update` fetches from |
| `netlog` | `off`: don't send the log, even with `net.host` set |
| `ntp.server` | where the clock comes from (SNTP): an IPv4 address or a name. Without it, the network's gateway, then `pool.ntp.org` if the gateway gives no time |
| `ntp` | `off`: don't set the clock from the network (it stays the real-time clock's, as `rtc` says) |
| `terminal.font` | the terminal windows' text: `smooth` (JetBrains Mono, the default) or `bitmap` (the 8x16 font); from when the stick is mounted. The screen without the desktop (`nocomp`) always has the 8x16 font |

## The clock from the network

Once the network has an address, sntp
asks the time (SNTP, UDP port 123) and sets the clock, then asks again
every hour; `date -r` says whether the clock came from the network or
from the PC's real-time clock, and the log has a line with how far off
the clock was ([ARCHITECTURE.md](../ARCHITECTURE.md#time-and-settings)).

## Files and speed over the network

(TCP; the PC's address is in `net`,
10.2.21.241 on the owner's network, and the Mac's here is 10.2.21.174):

- **A file from the Mac.** In a folder on the Mac,
  `python3 -m http.server 8000` (any web server will do); on the PC,
  `cd /data` (`/boot` is read-only), then
  `fetch http://10.2.21.174:8000/<file>`. A name works as well as an
  address. fetch says the size, a progress line every 2 s, and at the end
  the bytes, the time and the speed (MB/s, 10^6 bytes a second). The file
  is written as `<file>.part` and renamed once it is whole, so a fetch
  that fails or is stopped (Ctrl+C) leaves nothing. It follows up to 3
  redirects (to `http://` only) and takes Content-Length, chunked and
  until-closed bodies; a head over 16 KiB, no whole head in 20 s or a body
  silent for 30 s ends it (exit 1). `fetch <url> | head` writes into the
  pipe (at most 4 MiB, the pipe's size). Into a file, the speed is the
  stick's writing speed.
- **A file to the Mac.** On the PC `serve /data/big.bin` (port 8080;
  `serve /data/big.bin 9000` for another, or `serve /data/big.bin 80` for
  the web's own, so `curl http://10.2.21.241/` needs no port). It serves in the
  background (bin/serve, a service init runs), so the shell is free at
  once. On the Mac:

  ```sh
  curl http://10.2.21.241:8080/ -o big.bin
  ```

  Every path gets that file and nothing else: no folder listing, nothing
  from the request is ever opened (GET and HEAD, one byte range, so
  `curl -C -` resumes; a browser works too). Up to 4 files on 4 ports and
  24 clients at once (16 a file), each cut off after 30 s without
  progress; each request is a line in the log. `serve` alone lists what is
  served (file, port, clients, requests, bytes sent); `serve stop` stops
  everything, `serve stop 9000` one. bin/serve holds the listen
  permission for every port, those below 1024 too (`svc net listen low`
  in its list: no other program has it, and no program on `/data` can
  be allowed it), and no mount: the shell opens the file and hands it
  over. When the network has no address yet, `serve` says so after 5 s.
- **Throughput.** On the Mac, from the repository:

  ```sh
  python3 tools/speed.py server
  ```

  (TCP and UDP port 5201; allow incoming connections if macOS asks). On
  the PC: `speed 10.2.21.174` (it sends for 5 s; `-t 10` for 10),
  `speed 10.2.21.174 -r` (the Mac sends), `speed 10.2.21.174 -u` (UDP
  datagrams to the Mac, the lost ones counted). The other way round: on
  the PC `speed -l`, then on the Mac
  `python3 tools/speed.py client 10.2.21.241` (it sends) and
  `python3 tools/speed.py client 10.2.21.241 -r` (the PC sends). Both
  sides say MB/s and Mbit/s. Ctrl+C stops `speed` (`speed -l` too); a port
  nothing listens on is said to be refused.

## The log on the Mac

With `net.host` set, netlog sends each boot's
whole log, from its first line, over UDP to that address (port 5021), and
after a panic the panicked boot's log too. On the Mac, from the
repository, leave this running:

```sh
python3 tools/netlog-recv.py ~/jamos-logs
```

It listens on port 5021 (allow incoming connections if macOS asks),
prints the lines as they come, and writes one file per boot into
`~/jamos-logs`: `boot-<the boot's start, local time>.txt`, and
`boot-<...>-lastcrash.txt` for the log of a boot that panicked before
it, each with a `.pos` file beside it (how much it has, so a restarted
receiver goes on where it stopped). A receiver started late still gets
the boot from its first line. `--quiet` doesn't print the lines;
`--from <the PC's address>` takes datagrams only from it.

## A new build without moving the stick

Updates are signed: the PC
takes only a build whose manifest was signed with your key, and a build
made without the key takes no update at all.

0. Once, on the Mac, make the key:

   ```sh
   make
   build/host/jamos-sign keygen
   ```

   That writes `~/.config/jamos/update.key` (the secret: mode 0600, never
   in the repository; back it up) and `update.pub` (its public half, which
   every later `make` builds into the boot image). It refuses to replace
   a key that is already there. Then `make` and **`make flash`**: the
   build on the stick must already have the key before `update` works, and
   the build the stick has now accepts no signed manifest at all, so this
   first signed build goes on by hand. If the key is ever lost, make a new
   one (delete the two files first) and `make flash` again: builds signed
   with the old key are refused from then on, and the other way round.
1. On the Mac: `make`, then leave this running:

   ```sh
   python3 tools/update-server.py
   ```

   It serves `build/jamos.elf`, `build/bootfs.img` and the boot menu
   `boot/limine.conf` (`--no-menu`: none) on UDP port 5022,
   each snapshot's manifest signed with `~/.config/jamos/update.key`
   (`--key <file>` for another; it won't start without one), `--build
   <dir>` for another folder, `--client <the PC's address>` to answer only
   the PC. Run `make` again whenever you like: the next `update` gets the
   new build. Let `make` finish first, or the kernel and the boot image
   can come from two builds.
2. On the PC: `update`. It fetches the build from `net.host`, init checks
   the manifest's signature against the key in the running build, then
   each file's size and SHA-256 against the manifest, the screen says
   `old -> new` (version and git commit), and init loads the build into
   memory and writes it to the stick. Then it stops; its last line says
   what comes next:

   ```
   update: written to the stick and loaded: `reboot` starts it now, `reboot -f` restarts through the firmware (the stick boots it too)
   ```

   `reboot` starts the new build from memory (kexec: a few seconds, no
   firmware); `reboot -f` restarts through the firmware and the boot menu,
   and the stick boots the new build too, as does every power-on after.
   A build with another network default (the Mac's tree without
   `local.mk`, say) is refused: `update -f` takes it anyway.

### What `update` does to the stick

Once init has checked and loaded the
build, it writes it to the stick (only init can: no program, the shell
included, can write the boot partition), and the stick's own build stays
on as the boot menu's **"Jam OS (previous build)"**, which `make flash`
keeps the same way. The write takes a few seconds (about 20 on the
owner's USB 2 stick); the stick boots throughout (the old build first,
under every name, then the new one). If anything goes wrong the screen
says how far it got and that the new build is still loaded: `reboot`
runs it, while the stick (`reboot -f`, a power-off) still boots the old
one. A stick that has the build already gets nothing written ("the stick
has this build already"; "this is the build running now" if it is the
running one too), and the build is loaded all the same. The update brings
the boot menu too: new entries in `boot/limine.conf` reach the stick
without `make flash`. init writes the menu after the build, only if it
passes init's check (its default entry boots the new build, "Jam OS
(previous build)" the previous one, every file it names is on the stick;
`make check` runs the same check on `boot/limine.conf`), keeps the
stick's old menu as `/esp/boot/limine/limine.conf.prev`, and the stick
has a whole menu at every moment; a menu that fails is left out and the
screen says why.

### The other ways

`update -m` loads the build into memory only and
leaves the stick alone, for a build you'd rather try first: `reboot` runs
it and keeps running it (it survives `reboot` and a panic), and a
power-off or `reboot -f` brings back the stick's. `update -n` fetches and
checks only: nothing loaded, nothing written. `update -w` is the same as
plain `update` (it was the stick write's flag when plain `update` wrote
nothing). `update -r` does the plain update and then reboots into it at
once (`update -m -r`: into memory only, then the reboot), unless the
stick write failed. `update -m` and `update -n` never touch the stick or
its menu. With nothing new loaded, `reboot` and `reboot -f` are as they
always were: the stick's build. Every form needs a build with the key. A
signature proves the build is one you signed, not that it is the newest:
an older signed build is accepted too (the versions are printed).
