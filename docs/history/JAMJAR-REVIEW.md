# jamjar review

An independent read of the music player's window at commit 46c1393:
`user/apps/jamjar` (all of it), the player's side (`user/services/music`:
`spectrum.c`, `prev`, `play`, `pause`, `sleep`, `levels`, `spectrum`;
`abi/idl/music.idl`), the shell's `music` and `jamjar` commands and the
RUN_MUSIC path in `sh_program.c`, what it added to libfun (UTF-8 text,
`font_latin`, `text_clip`, `ring_aa`, `poly_aa`, the new keys) and libos
(`strrchr`, `strstr`), and how stb_image is built and fed. Findings first;
each one's outcome is filled in as it is fixed.

Severity: **High** is the owner's bug, or a program reaching what it
should not; **Medium** is wrong behaviour a user will meet, or a test that
fails for a reason that is not the code's; **Low** is a wart or a narrow
race.

What held up: the ID3 parser bounds every length against what is left
(2.2/2.3/2.4, unsynchronisation, extended headers, footers, the data
length indicator) and the self-test fuzzes it; stb_image only ever sees a
picture whose size was checked first, and all of its memory comes from a
bounded arena; the player's `prev` history, pause, sleep fade and
spectrum ring are consistent, and the spectrum is answered at the frame
the mixer says is heard; the link thread keeps every call off the UI
thread; the UTF-8 decoder never reads past a NUL and draws one box per
malformed byte.

## The owner's bug: now playing loses its cover

On the PC (2560x1440, 93 albums, every MP3 with a ~640x640 PNG cover, in
shuffle) the now-playing card sometimes shows the album's cover and
sometimes its jar label, while the big view (`f`) shows the cover.

**Cause: the art cache (art.c) is bounded by a count, not by bytes, and
it lives in libos's 16 MiB heap.** It keeps every picture it draws, 64 of
them, each `size * size * 4` bytes from `malloc`, and it takes a free
slot before it evicts anything. At 2560x1440 now playing's cover is 435
px: 757 KB. Each album played leaves two such entries (the 256 px copy
scaled up while the 512 px one is read, `COVER_SMALL_KIND`, and then the
512 px one, `COVER_LARGE_KIND`: the cache is keyed by kind), and drawing
one needs a third, `cover_render`'s scratch buffer of the same size. With
the pool's stacks and the library also in the heap, after about ten
albums the heap is full: `art_cover`'s `malloc` or `cover_render`'s fails,
and `art_cover` draws the jar label ("not kept"), every frame, for every
album it has not cached already. An album played earlier still shows its
cover (a cache hit needs no memory): hence "sometimes". The big view's
cover is 230 px, a quarter of the bytes, so it still fits. The cover
thread (cover.c) had read and decoded the cover all along: its state
machine was not the problem.

Reproduced in QEMU by `tools/jamjar-covers-test.sh` (20 albums, two songs
each, teal covers, shuffle, 16 skips, two shots after each): at 46c1393
the shots after skips 11-14 show the jar label (album 07, whose cover the
log says was read "640x640 ... (large too)"), and the shots of skip 15
show a cover again (album 16, played before). QEMU's 1280x800 draws now
playing's cover at 216 px, 187 KB, so `tools/jamjar-test.sh` (7 albums)
never got near the limit.

## Findings

| # | Sev | Where | What |
|---|---|---|---|
| 1 | High | `user/apps/jamjar/art.c:135-168`, `cover.c:222-253` | The owner's bug, above: the art cache's 64 entries are not bounded in bytes, a kind change keeps the old entry beside the new one, and a miss allocates a second full-size buffer; at 2560x1440 the 16 MiB heap runs out after about ten albums and now playing draws the jar label every frame for every album not drawn before. The failing path also re-allocates and redraws a 435 px label every frame. |
| 2 | Medium | `user/apps/jamjar/art.c:153-157`, `cover.c:193-194,229-232` | A cover can vanish for a frame: `cover_ready` reports which image is ready and `cover_render` reads it later under a second hold of the lock; if the cover thread gave that image's slot to another album in between (`claim`), the render fails and the jar label is drawn instead of the picture that was there. Large copies are taken from each other when more than two albums are drawn bigger than 288 px: not at 2560x1440 (only now playing is), but at 3840x2160 the roulette's middle label and the big view are too. |
| 3 | Medium | `user/apps/jamjar/roulette.c:14-33`, `tools/shell-tests/jamjar.txt:89` | The roulette can land on the album already playing. The app then plays that album again from a new shuffle; the test waits for a new "hearing" line and fails when none comes (about 1 run in 5, seen by the main session). |
| 4 | Medium | `user/apps/jamjar/link.c:123-152`, `main.c:85`, `nowplaying.c:8-15` | When the player stops answering (it crashed and init has not restarted it yet, or it hangs), the view keeps its last state: "NOW PLAYING", the progress bar running on to the track's end, the bars frozen at the last bands heard. `answered` is set false but nothing reads it. |
| 5 | Medium (design) | `user/apps/jamjar/stbi.c`, `cover.c:327-352` | The covers are decoded from files on a stick by stb_image inside jamjar, which holds the player's channel, the mixer's and (M8 design question g) every mount read-write. stb_image has had fuzzer-found memory bugs before, and `STBI_ASSERT` is compiled out here. The arena bounds its memory, not its pointer arithmetic. See "Design questions". |
| 6 | Low | `user/apps/jamjar/nowplaying.c:150`, `input.c:97` | A volume of -0.5 dB is shown as "0.5 dB" (`cb / 10` is 0, so the sign is lost); the slider can be dragged to -5 centibels. |
| 7 | Low | `abi/idl/music.idl:72-73` | `levels`' bands are said to map -70..-10 dB to 0..255; they are the loudest of four `spectrum` bands, which map -76..-16 dB. |
| 8 | Low | `user/services/music/spectrum.c:1`, `music.h:153` | The header says sixteen bands (`levels`); it makes 64. `music.h` says the spectrum is 30 KiB; `struct spectrum` is about 85 KiB. |
| 9 | Low | `user/apps/jamjar/cover.c:26,147-164` | The cover table holds 1024 albums; the library up to 4096 files. Past 1024 albums the rest never get a cover, and every `cover_ready` for one of them walks the whole full table (1024 probes, about 20 times a frame). |
| 10 | Low | `user/apps/jamjar/main.c:45-46`, `cover.c:178-182,393` | An album's cover comes from the first file it is asked with (the read-ahead asks with the album's first track): if that file has no picture, the album keeps its label even while a track of it with a picture plays. A file that can't be read (the stick out for a moment) marks the album "no cover" until jamjar restarts. |
| 11 | Low | `user/apps/jamjar/link.c:167-206`, `kernel/object/channel.c:546` | jamjar shares the shell's client end of the player's channel. A call cut short (jamjar quits or is killed while the link thread waits for an answer, or a call times out) leaves the player's late answer queued on that endpoint, which the shell only ever calls on and never reads: up to 1024 messages kept in the shell's job for good. Quitting cleanly would need the link thread stopped and joined before exit; the rest is design question C. |
| 12 | Low | `user/apps/jamjar/link.c:200-205` | `L.snap.link` is set after the thread starts, outside the lock the thread copies the snapshot under. Harmless today (the thread never writes `link`), but it is a data race by the guide's rules. |
| 13 | Low | `user/apps/jamjar/cover.c:228-239` | `cover_render` scales a whole image (up to 512 px to the size drawn) while holding the cover table's spinlock, and the cover thread spins on it meanwhile; both locks (`cover.c`, `link.c`) are busy-wait spinlocks in user space with no yield. |
| 14 | Low | `user/apps/fun/pool.c:69,96-110` | (libfun, used by jamjar on every frame) After each batch every worker spins about 1 ms before it sleeps; jamjar runs two batches a frame at 60 frames a second, so on the PC's 28 CPUs about 3 CPUs' worth of time goes to spinning while jamjar is open. |
| 15 | Low | `user/services/music/tracks.c:230-247` vs `user/apps/jamjar/library.c:165-185` | "In order" plays an album by byte order of the paths; jamjar lists tracks ignoring ASCII case, so the two orders differ when names differ only in case. |
| 16 | Low | `user/services/music/player.c:480-503` | `prev` before anything is heard (just after `play`) puts the track being written back on `ahead`, and it is remembered twice in the history when it opens again. |
| 17 | Low | `user/apps/fun/text.c:254-275` (`text_clip`) | With `max_w` narrower than "...", the dots are drawn past `max_w`. |
| 18 | Low | `user/tests/utest/music.c:14-15` | The utest `#include`s the player's `spectrum.c` (and `tracks.c`), which CODING-GUIDE forbids ("Never `#include` a `.c` file"). |

Not findings, checked: `id3.c`'s every length; `utf8_next` (overlongs,
surrogates, past U+10FFFF, a NUL mid-sequence); `strrchr`/`strstr`;
`ring_aa`/`poly_aa` clip to the surface; the bars' spans stay inside each
bar's own columns (the pool draws them in parallel); `name_*` and
`album_hash` bound their output; the library's walk and sort; the view's
indices; music.idl's new ordinals and `status`'s `playing` 3 (old clients
read non-zero as playing); the player's history, `ahead` stack, pause
around skips, the sleep fade and volume during it; `spec_cut` after
`audio_discard` (the write position goes back to what the mixer read);
the stb arena's size checks (an `n` near `SIZE_MAX` is refused before its
rounding is used).

## Design questions (for the owner; not decided here)

- **A (finding 5). Where covers are decoded.** Options: a small decoder
  process with no handles at all (it gets the tag's bytes, answers
  pixels in a VMO), started by jamjar per library; or keep it in jamjar
  and accept that a crafted picture on a stick could act with jamjar's
  handles; or narrow jamjar's handles (a read-only view of the music
  folder only) when M8's question g is decided.
- **B (finding 10).** Should an album's cover be the playing track's
  own picture when it has one (a cover per file, more reads), or stay
  one per album?
- **C (finding 11).** jamjar uses the shell's own client end of the
  player's channel. A `connect` method on music.idl (the player hands out
  a new channel per client) would give jamjar an endpoint of its own, so
  nothing it leaves behind lands in the shell.
