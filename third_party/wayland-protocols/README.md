# Wayland's interface files

Upstream's XML descriptions of the Wayland protocol, vendored unmodified as
data for `tools/genwl.py`, which generates Jam OS's tables and stubs
(`user/include/jwl/`, `user/lib/jwl_*.c`) from them. No libwayland code is
used. See docs/G1-PLAN.md, "The protocol".

| File | From | Upstream path | sha256 |
|---|---|---|---|
| `wayland.xml` | wayland 1.26.0 (tag `1.26.0`, commit 87cc8a8728a923fc57938faa81ba0e74f34ecdc7, 2026-07-16), https://gitlab.freedesktop.org/wayland/wayland | `protocol/wayland.xml` | cc860987e54f8d85c940e97fa1270c69b6e4ad31fbcf5a7f00107ce1157f5e07 |
| `xdg-shell.xml` | wayland-protocols 1.49 (tag `1.49`, commit ee78491a237eaff9389a0ccf8680521d074407d3, 2026-06-07), https://gitlab.freedesktop.org/wayland/wayland-protocols | `stable/xdg-shell/xdg-shell.xml` | 7ba7f9c8473deee674cb1f154a18abd0bb0cc072604fc055b0c15e459fc4c7df |
| `cursor-shape-v1.xml` | wayland-protocols 1.49 (the same tag and commit) | `staging/cursor-shape/cursor-shape-v1.xml` | bb57d91e53a79dadab7c612dab87c233393cee73673feefa7442cfbfdd9aed2f |
| `COPYING-wayland` | wayland 1.26.0 | `COPYING` | 6eefcb023622a463168a5c20add95fd24a38c7482622a9254a23b99b7c153061 |
| `COPYING-wayland-protocols` | wayland-protocols 1.49 | `COPYING` | f1a2b233e8a9a71c40f4aa885be08a0842ac85bb8588703c1dd7e6e6502e3124 |

Both are the latest stable releases when fetched (2026-10-05; tags
ending .9x are release candidates). Each file was fetched from its tag's
raw URL (`<project>/-/raw/<tag>/<path>`). cursor-shape-v1 is a staging
protocol (testing upstream, its version may grow); it names
`zwp_tablet_tool_v2` from tablet-v2, which is not vendored (no tablets):
`tools/genwl.py` generates that argument untyped (its FOREIGN list).

Licence: MIT (the "Expat" text X.org uses), in both COPYING files and in
each XML file's own `<copyright>` block. The generated files carry that
block too, as the licence asks of copies of substantial portions.

To take a newer release: replace the files, update this table and
`third_party/VERSIONS.md`, run `make wl`, and read the generated diff (a new
upstream version only adds messages at the end, so existing opcodes must
not move; `tools/genwl.py` refuses XML it doesn't understand).
