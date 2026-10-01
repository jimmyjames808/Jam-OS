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
  domain (Unlicense), the user's choice. Built into bin/jamjar only
  (user/apps/jamjar/stbi.c): PNG and JPEG from memory, no stdio, no HDR,
  no thread-locals (Jam OS has no TLS); its <stdlib.h> and <string.h> are
  user/apps/jamjar/port. stb_image is not written for hostile input, so
  jamjar checks a picture's size with stbi_info before decoding it
  (2048x1600 pixels at most) and gives it one bounded arena (40 MiB) for
  all of its memory: a picture that needs more fails to decode.
