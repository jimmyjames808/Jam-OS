Vendored third-party code:

- limine/: Limine v11.x-binary @ 5be26a7 (BSD-2-Clause)
- limine-protocol/: limine.h @ 3a0526b (0BSD)
- spleen/: Spleen 8x16 BDF (BSD-2-Clause)
- fatfs/: FatFs R0.16 (July 22, 2025) by ChaN, from
  http://elm-chan.org/fsw/ff/arc/ff16.zip (sha256
  99f7dc1f7e095356e4a9e3dbe29959090d8b948afe2bbc5441e52fdf4b85449e): its
  LICENSE.txt and source/ exactly as released (CRLF line ends kept; the
  documents/ folder left out), plus the author's two published patches to
  ff.c (http://elm-chan.org/fsw/ff/patches.html), applied in order:
  patch/ff16p1.diff (2025-09-13: smaller minimum volume; sha256
  7996ddc3135f3d53...) and patch/ff16p2.diff (2026-07-10: the fixes for
  CVE-2026-6682, -6687 and -6683: FAT size, exFAT label length and cluster
  count checks; sha256 5cd39f1fc299f0f1...). ff.c's banner reads "R0.16
  w/patch 2". BSD-style, one clause. Jam OS's
  configuration is user/services/fat/ffport/ffconf.h, not source/ffconf.h.
- dr_mp3/: dr_mp3 v0.7.4 (unreleased, "TBD" in its banner) by David Reid,
  based on minimp3 by lieff; dr_mp3.h from https://github.com/mackron/dr_libs
  at commit 51e61d308dde6b437fce0c5fabb32cd86b40f4d7 (2026-08-30, the last
  commit to touch dr_mp3.h; master was dfe8377 when fetched on 2026-10-01
  with the same file), sha256 997b7ee18de6e6b8...; unmodified. Chosen over
  the last release, v0.7.3 (tag mp3-0.7.3, 2026-01-17), because four of the
  six commits since fix an out-of-bounds read and overflows in its Xing/Info
  tag parsing, which every LAME-encoded file reaches. Its LICENSE (the
  repository's, sha256 dd1c647e6f767f8f...) is kept beside it; the same
  text ends dr_mp3.h: public domain (Unlicense) or MIT No Attribution, the
  user's choice. Jam OS's configuration (no stdio, SSE2 on, 16-bit
  output, Layers I-III) is user/lib/mp3port/dr_mp3_impl.c, built into
  libos for <mp3.h>.
- pl_mpeg/: PL_MPEG (MPEG-1 video, MP2 audio, MPEG-PS demuxer) by Dominic
  Szablewski, https://github.com/phoboslab/pl_mpeg at commit c871f2b
  (2025-12-30, "Fix corrupt slice check"): pl_mpeg.h exactly as released
  (sha256 3a8cb30c83c2a114...), MIT. The header carries only an SPDX line
  since upstream commit 16a6a11; LICENSE holds the full MIT text it
  carried before. Built into bin/splash only (user/apps/splash/plmpeg.c,
  PLM_NO_STDIO; its <string.h> and <stdlib.h> are user/apps/splash/port).
- stb_image/: stb_image v2.30 (2024-05-31) by Sean Barrett, stb_image.h
  from https://github.com/nothings/stb at commit
  f75e8d1cad7d90d72ef7a4661f1b994ef78b4e31 (2024-07-29, master then;
  taken from vcpkg's checkout of that commit, which it verified by the
  archive's SHA-512), sha256 594c2fe35d49488b...; unmodified. LICENSE
  (sha256 bebfe904b1430165...) is the repository's: MIT or public
  domain (Unlicense), the user's choice. Built into bin/jamcover only
  (user/apps/jamcover/stbi.c), jamjar's cover helper, a process that
  holds nothing but the picture and its pixels: PNG and JPEG from memory,
  no stdio, no HDR, no thread-locals (Jam OS has no TLS); its <stdlib.h>
  and <string.h> are user/apps/jamcover/port. stb_image is not written
  for hostile input, so the helper checks a picture's size with stbi_info before decoding it
  (2048x1600 pixels at most) and gives it one bounded arena (40 MiB) for
  all of its memory: a picture that needs more fails to decode.
- stb_truetype/: stb_truetype v1.26 (2021-08-28) by Sean Barrett,
  stb_truetype.h from https://github.com/nothings/stb at commit
  2c980bb59875b0d32144a71867fbdebb2f77cd20 (2026-08-02, master then; the
  last commit to touch the file is 6e9f34d, 2024-07-15, and the file is
  byte for byte the one at stb_image's commit above), sha256
  ecd30b05e0dd4fea...; unmodified. LICENSE is the repository's (the same
  file as stb_image's): MIT or public domain (Unlicense), the user's
  choice. Built into libfun (user/apps/fun/ttf.c: no stdio, its maths and
  memory from libfun and libos) for the smooth text (user/apps/fun/font.c),
  and with the Mac's compiler into build/host/fontpreview. It runs only
  when a font is opened, on the built-in font below: stb_truetype doesn't
  check a font's offsets, so it is never given a font from anywhere else.
- inter/: Inter 4.1 by Rasmus Andersson and the Inter Project Authors,
  SIL Open Font License 1.1 (LICENSE.txt, as released; no Reserved Font
  Name). From the release archive
  https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip
  (sha256 9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e;
  fetched 2026-10-05): extras/ttf/Inter-Regular.ttf (sha256
  40d692fce188e447...) and extras/ttf/Inter-Medium.ttf (97ad806f526e4154...),
  version string "Version 4.001;git-9221beed3". Cut down for libfun's
  smooth text with fontTools 4.60.2 (tools/subsetfont.py: printable ASCII,
  U+00A0..U+00FF, nine punctuation marks (U+2013 U+2014 U+2018 U+2019
  U+201C U+201D U+2022 U+2026 U+20AC) and .notdef; no hinting; the GPOS
  kerning flattened into a 'kern' table that stb_truetype reads, checked
  against HarfBuzz's shaping of the upstream font for 210 pairs; GPOS,
  GSUB and GDEF dropped), from the repository root:

      python3 tools/subsetfont.py <archive>/extras/ttf/Inter-Regular.ttf third_party/inter/Inter-Regular.ttf
      python3 tools/subsetfont.py <archive>/extras/ttf/Inter-Medium.ttf third_party/inter/Inter-Medium.ttf

  which give Inter-Regular.ttf (45772 bytes, 222 glyphs, 4773 kerning
  pairs, sha256 a98ba86b60e4726b...) and Inter-Medium.ttf (48892 bytes, 222
  glyphs, 5299 pairs, sha256 4c5373e4e2d19173...); the same input gives the
  same bytes. Linked into libfun as they are (user/apps/fun/fontdata.c).
  A modified version under the OFL, which allows bundling with any
  software; the fonts are not sold by themselves.
- lwip/: lwIP 2.2.1 (the latest 2.2.x release), tag STABLE-2_2_1_RELEASE,
  commit 77dcd25a72509eb83f72b033d219b1d40cd8eb95 (tag object 009c225),
  from https://github.com/lwip-tcpip/lwip (the Savannah repository's
  mirror; the same tag is at git.savannah.nongnu.org/git/lwip.git). The
  release archive https://download.savannah.nongnu.org/releases/lwip/lwip-2.2.1.zip
  (sha256 7b622662dba2383d71f874f2e494b54ae948531559c17acbe75797966d646878)
  has the same src/ but with CRLF line ends; the files here are the tag's
  (LF), unmodified. COPYING is lwIP's licence, BSD-3-Clause (sha256
  ef4aac92e05e87cd...). Only what netstack compiles: src/core (def,
  inet_chksum, init, ip, mem, memp, netif, pbuf, raw, stats, tcp,
  tcp_in, tcp_out, timeouts, udp), src/core/ipv4 (etharp, icmp, ip4,
  ip4_addr), src/netif/ethernet.c, and the headers those include (found
  from the build's dependency files); IPv6, IP fragments, IGMP, DHCP, DNS,
  the sockets and netconn APIs and apps/ are left out. The TCP files were
  added from the same tag (checked byte for byte against the release
  archive above, line ends aside). Jam OS's configuration (NO_SYS,
  the options file, the clock, its <string.h>) is
  user/services/netstack/port. Known and handled outside lwIP: its ARP
  input reads the 28-byte header without checking the frame holds it, so
  netstack pads every short frame with zeros to 60 bytes first
  (user/services/netstack/stack.c, tested by utest's netstack_malformed).
- monocypher/: Monocypher 4.0.2 by Loup Vaillant and its contributors,
  the release archive https://monocypher.org/download/monocypher-4.0.2.tar.gz
  (sha256 38d07179738c0c90677dba3ceb7a7b8496bcfea758ba1a53e803fed30ae0879c;
  its SHA-512 is the one monocypher.org publishes beside it). Only what
  signed updates need, unmodified: src/monocypher.c and .h (sha256
  afe2b098c8569577... and f78bb31255cfb7be...), src/optional/monocypher-ed25519.c
  and .h (7c9b16056cbd2752... and bd546edcd468d64e...: Ed25519 as RFC 8032
  has it, EdDSA with SHA-512, which is what Jam OS uses, not Monocypher's
  default EdDSA with BLAKE2b), and LICENCE.md (5f8360e4c06ddcc5...).
  Dual-licensed BSD-2-Clause or CC0, the user's choice: Jam OS takes it
  under BSD-2-Clause. tests/vectors-ed25519.h is a cut of the archive's
  tests/vectors.h (sha256 5cc8b311b0a23b65...): its first 5 lines and its
  three Ed25519 tables (lines 10976-13842: ed_25519, ed_25519_pk,
  ed_25519_check; official vectors, RFC 8032's among them, and random ones
  from libsodium and ed25519-donna), unchanged. Built into libos
  (user/lib/updsig.c: init's check of an update's signature, and utest)
  and, with the Mac's compiler, into build/host/jamos-sign
  (tools/jamos-sign.c: the key and the signatures; its `self-test` runs
  those vectors).
- wayland-protocols/: Wayland's interface files, vendored unmodified as data
  for tools/genwl.py (no libwayland code is used): wayland.xml from wayland
  1.26.0 (tag 1.26.0, commit 87cc8a8728a923fc57938faa81ba0e74f34ecdc7,
  protocol/wayland.xml, sha256 cc860987e54f8d85...) and xdg-shell.xml from
  wayland-protocols 1.49 (tag 1.49, commit
  ee78491a237eaff9389a0ccf8680521d074407d3, stable/xdg-shell/xdg-shell.xml,
  sha256 7ba7f9c8473deee6...), both from https://gitlab.freedesktop.org/wayland/.
  MIT: each project's COPYING is kept beside them (COPYING-wayland,
  COPYING-wayland-protocols); README.md there has every file's source and
  sha256. The generated user/include/jwl/*.h and user/lib/jwl_*.c carry the
  XML's copyright block.
