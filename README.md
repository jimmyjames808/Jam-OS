<h1 align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/logo/jamos-lockup-dark.png">
    <img alt="Jam OS" src="docs/logo/jamos-lockup-light.png" width="420">
  </picture>
</h1>

Jam OS is a from-scratch operating system for x86_64 PCs, written in C.
Every driver and service is a separate process that can only use the
handles it is given, so a crashed driver doesn't take the system down. It
boots from a USB stick on a real PC into a desktop of its own.

## A tour

![The Jam OS desktop at 2560x1440, tiled: a terminal listing the running programs (ps) on the left; Jamjar, the music player, at the top right, playing a Chopin nocturne with its library of four composers and six albums, the album cover, the controls and stereo spectrum bars; a second terminal under it showing uname -a and the music folder; the top bar across the top with the window chips and the clock](docs/images/desktop-tiled.png)

The desktop, tiled: each new window splits the one in focus, here two
terminals and Jamjar, the music player, playing from a USB stick.

![The search box open over the same tiled desktop, centred in the picture: "ja" typed, Jamjar as the top result with "Music player" and "Enter" beside it, and a row to run "ja" in a terminal; Jamjar's library and the top bar's window chips around it](docs/images/desktop-search.png)

The search box: tap Super (or click "Jam OS" on the top bar) to start an
app or run a command in a new terminal.

![The same three windows floating on the jam wallpaper, apart with gaps between them: a terminal listing the running programs on the left, Jamjar playing at the top right with the raspberry, apricot and blackcurrant circles on the left of its title bar, and a smaller terminal below it; rounded corners and soft shadows](docs/images/desktop-floating.png)

The same windows floating (Super+T switches a screen): the jam circles
on each title bar close, minimise and go full screen.

## What works today

- **Desktop**: a Wayland compositor of its own, tiling and floating
  windows, virtual screens, search, notices, terminals with copy and
  paste, and Jamjar, a music player.
- **Kernel**: preemptive SMP on all 28 CPUs of the PC, capability handles,
  processes and jobs with quotas, reboot by kexec, a calm panic screen.
- **Drivers**: user processes, restarted when they crash, behind the
  IOMMU (Intel VT-d): USB, Intel HD Audio, the Realtek RTL8125B network chip.
- **Storage**: FAT32 on USB sticks.
- **Network**: DHCP, DNS, UDP and TCP, files over HTTP, and updates
  straight from the Mac.
- **Testing**: kernel and user-space test suites, stress and soak tests,
  benchmarks.

## Try it

On macOS, with [Homebrew](https://brew.sh):

```sh
brew install x86_64-elf-gcc qemu mtools
make run
```

On a real PC: `make usb DEV=/dev/diskN` writes a stick (it erases the
whole disk), then boot it in UEFI mode with Secure Boot off. More in
[docs/HARDWARE.md](docs/HARDWARE.md).

## What's next

NVMe drives, then POSIX. The plan: [docs/ROADMAP.md](docs/ROADMAP.md).

## Documentation

- [docs/USING.md](docs/USING.md): the desktop, its keys and the shell
- [ARCHITECTURE.md](ARCHITECTURE.md): how it is designed, and why
- [CODING-GUIDE.md](CODING-GUIDE.md): how the code is written, and the build commands
- [docs/TESTING.md](docs/TESTING.md): the tests
- [docs/HARDWARE.md](docs/HARDWARE.md): the PC, and flashing the stick
- [docs/NETWORK.md](docs/NETWORK.md): the network and `update`
- [docs/ROADMAP.md](docs/ROADMAP.md) and [docs/HISTORY.md](docs/HISTORY.md): what's next and what's done

## Licence

[BSD 2-Clause](LICENSE). The code in `third_party/` keeps its own licences
(Limine: BSD-2-Clause; `limine.h`: 0BSD; Spleen: BSD-2-Clause; FatFs: its
own one-clause BSD-style licence; dr_mp3: public domain or MIT-0;
pl_mpeg: MIT; stb_image and stb_truetype: MIT or public domain; Inter and
JetBrains Mono: SIL Open Font License 1.1; Wayland's protocol files: MIT;
lwIP: BSD-3-Clause; Monocypher: BSD-2-Clause, or CC0). The music in the
pictures is public-domain recordings from the Internet Archive (Musopen
and others); none of it is in this repository.
