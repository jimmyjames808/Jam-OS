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
