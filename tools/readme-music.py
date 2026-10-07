#!/usr/bin/env python3
"""A small music library for the README's Jamjar pictures (tools/readme-shots.sh).

    readme-music.py <dir>

Downloads the public-domain recordings below from the Internet Archive
(each file's MD5 checked against the item's own list), and lays them out
in <dir>/music as music libraries are, and as Jamjar reads them:
Artist/Album/NN - Title.mp3 (no commas: mtools writes them as '_').
Each album gets a cover made here (its name
and its composer on a field in the jam colours, drawn with Pillow and the
Inter font from third_party/), put into every track's ID3v2 tag by
ffmpeg (the audio is copied, not re-encoded). The recordings' own tags
are dropped: Jamjar names everything from the folders and files.

The downloads are kept in <dir>/download, so a second run fetches
nothing. <dir> must be outside the repository: no music, downloaded or
not, is ever committed. Needs network access, Pillow and ffmpeg.

The recordings and their licences (each checked on its item's page,
https://archive.org/details/<item>):
  musopen-chopin                    Musopen, "The Complete Chopin
                                    Collection": CC0 1.0
  karine-gilanyan-beethoven-piano-sonata-nr.-15-in-d-major-op.-28-pastoral
                                    Karine Gilanyan (piano), for Musopen:
                                    Public Domain Mark 1.0
  SymphonyNo.5                      a Musopen recording, released into the
                                    public domain: Public Domain Mark 1.0
  mozart-symphony-no-40-k-550-pd36  the Musopen Symphony: Public Domain
                                    Mark 1.0
  Mussorgskys_Pictures_at_an_Exhibition-2471
                                    Skidmore College Orchestra (via the
                                    Free Music Archive, for Musopen):
                                    Creative Commons public domain
                                    dedication
"""
import hashlib
import json
import os
import subprocess
import sys
import urllib.parse
import urllib.request

from PIL import Image, ImageDraw, ImageFilter, ImageFont

CHOPIN = "musopen-chopin"
GILANYAN = "karine-gilanyan-beethoven-piano-sonata-nr.-15-in-d-major-op.-28-pastoral"
FIFTH = "SymphonyNo.5"
MOZART = "mozart-symphony-no-40-k-550-pd36"
SKIDMORE = "Mussorgskys_Pictures_at_an_Exhibition-2471"
GIL = "Karine Gilanyan - Beethoven - Piano Sonata nr.15 in D major op.28 Pastoral - "
MOZ = "Mozart_-_Symphony_No._40_in_G_minor,_K550_-_"
SKI = "Skidmore_College_Orchestra_-_"

# (artist, album, the cover's two colours, [(item, file, title)])
ALBUMS = [
    ("Frédéric Chopin", "Ballades", (0xd4537e, 0x7f77dd), [
        (CHOPIN, "Ballade no. 1 - Op. 23.mp3", "Ballade No. 1 in G minor"),
        (CHOPIN, "Ballade no. 2 - Op. 38.mp3", "Ballade No. 2 in F major"),
        (CHOPIN, "Ballade no. 3 - Op. 47.mp3", "Ballade No. 3 in A-flat major"),
        (CHOPIN, "Ballade no. 4 - Op. 52.mp3", "Ballade No. 4 in F minor"),
    ]),
    ("Frédéric Chopin", "Nocturnes", (0x7f77dd, 0x161b26), [
        (CHOPIN, "Nocturne Op. 27 no. 1 in C sharp minor.mp3", "Nocturne in C-sharp minor"),
        (CHOPIN, "Nocturne Op. 32 no. 1 in B major.mp3", "Nocturne in B major"),
        (CHOPIN, "Nocturne Op. 48 no. 1 in C minor.mp3", "Nocturne in C minor"),
        (CHOPIN, "Nocturne Op. 55 no. 2 in E flat major.mp3", "Nocturne in E-flat major"),
    ]),
    ("Ludwig van Beethoven", "Piano Sonata No. 15 (Pastoral)", (0xef9f27, 0xd4537e), [
        (GILANYAN, "01 " + GIL + "I. Allegro.mp3", "I. Allegro"),
        (GILANYAN, "02 " + GIL + "II. Andante.mp3", "II. Andante"),
        (GILANYAN, "03 " + GIL + "III. Scherzo. Allegro Vivace.mp3",
         "III. Scherzo. Allegro vivace"),
        (GILANYAN, "04 " + GIL + "IV. Rondo. Allegro ma non troppo.mp3",
         "IV. Rondo. Allegro ma non troppo"),
    ]),
    ("Ludwig van Beethoven", "Symphony No. 5", (0xd4537e, 0x161b26), [
        (FIFTH, "Ludwig_van_Beethoven_-_symphony_no._5_in_c_minor_op._67_-_i._allegro_con_brio.mp3",
         "I. Allegro con brio"),
    ]),
    ("Modest Mussorgsky", "Pictures at an Exhibition", (0x3f9a8a, 0x7f77dd), [
        (SKIDMORE, SKI + "01_-_Promenade_Allegro_giusto_nel_modo_russico_senza_allegrezza_ma.mp3",
         "Promenade"),
        (SKIDMORE, SKI + "02_-_I_Gnomus_Vivo.mp3", "Gnomus"),
        (SKIDMORE, SKI + "03_-_Promenade_Moderato_comodo_e_con_delicatezza.mp3", "Promenade"),
        (SKIDMORE, SKI + "04_-_II_Il_vecchio_castello_Andante.mp3", "Il vecchio castello"),
        (SKIDMORE, SKI + "06_-_III_Tuileries_Allegretto_non_troppo_capriccioso.mp3", "Tuileries"),
        (SKIDMORE, SKI + "07_-_IV_Bydlo_Sempre_moderato_pesante.mp3", "Bydło"),
    ]),
    ("Wolfgang Amadeus Mozart", "Symphony No. 40", (0xef9f27, 0x3f9a8a), [
        (MOZART, MOZ + "III._Menuetto._Allegretto_(Musopen_Symphony).mp3",
         "III. Menuetto. Allegretto"),
        (MOZART, MOZ + "IV._Finale._Allegro_assai_(Musopen_Symphony).mp3",
         "IV. Finale. Allegro assai"),
    ]),
]

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def rgb(c):
    return (c >> 16 & 255, c >> 8 & 255, c & 255)


def fetch(dl, item, name, meta_cache={}):
    if item not in meta_cache:
        with urllib.request.urlopen("https://archive.org/metadata/" + item) as r:
            meta_cache[item] = json.load(r)
    info = [f for f in meta_cache[item]["files"] if f["name"] == name]
    if not info:
        sys.exit("readme-music: %s has no file %s" % (item, name))
    dest = os.path.join(dl, item + "__" + name)
    want = info[0].get("md5")
    if os.path.exists(dest) and hashlib.md5(open(dest, "rb").read()).hexdigest() == want:
        return dest
    url = "https://archive.org/download/%s/%s" % (item, urllib.parse.quote(name))
    print("readme-music: fetching", url, flush=True)
    urllib.request.urlretrieve(url, dest + ".part")
    got = hashlib.md5(open(dest + ".part", "rb").read()).hexdigest()
    if got != want:
        sys.exit("readme-music: %s: MD5 %s, the item says %s" % (name, got, want))
    os.rename(dest + ".part", dest)
    return dest


def cover(path, artist, album, colours):
    """640x640: a diagonal blend of the album's two colours, a soft jar of
    lighter circles, the album's name and its composer in Inter."""
    n = 640
    a, b = rgb(colours[0]), rgb(colours[1])
    im = Image.new("RGB", (n, n))
    px = im.load()
    for y in range(n):
        for x in range(n):
            t = (x + y) / (2 * n - 2)
            px[x, y] = tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))
    glow = Image.new("L", (n, n), 0)
    d = ImageDraw.Draw(glow)
    d.ellipse([n * 0.42, n * 0.08, n * 1.02, n * 0.68], fill=70)
    d.ellipse([n * 0.58, n * 0.30, n * 0.88, n * 0.60], fill=110)
    glow = glow.filter(ImageFilter.GaussianBlur(18))
    im = Image.composite(Image.new("RGB", (n, n), (255, 255, 255)), im, glow)
    d = ImageDraw.Draw(im)
    big = ImageFont.truetype(os.path.join(REPO, "third_party/inter/Inter-Medium.ttf"), 50)
    small = ImageFont.truetype(os.path.join(REPO, "third_party/inter/Inter-Regular.ttf"), 30)
    words, lines = album.split(), [""]
    for w in words:   # the album's name wrapped to the cover's width
        t = (lines[-1] + " " + w).strip()
        if d.textlength(t, font=big) > n - 96 and lines[-1]:
            lines.append(w)
        else:
            lines[-1] = t
    y = n - 56 - 40 - 62 * len(lines)
    for line in lines:
        d.text((48, y), line, font=big, fill=(255, 255, 255))
        y += 62
    d.text((48, y + 8), artist, font=small, fill=(255, 255, 255, 200))
    im.save(path)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    top = os.path.abspath(sys.argv[1])
    if (top + "/").startswith(REPO + "/"):
        sys.exit("readme-music: %s is inside the repository: use a folder outside it" % top)
    dl, music = os.path.join(top, "download"), os.path.join(top, "music")
    os.makedirs(dl, exist_ok=True)
    for artist, album, colours, tracks in ALBUMS:
        folder = os.path.join(music, artist, album)
        os.makedirs(folder, exist_ok=True)
        art = os.path.join(top, "cover-%s-%s.png" % (artist, album))
        cover(art, artist, album, colours)
        for i, (item, name, title) in enumerate(tracks, 1):
            src = fetch(dl, item, name)
            dest = os.path.join(folder, "%02d - %s.mp3" % (i, title))
            if os.path.exists(dest):
                continue
            subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", src,
                            "-i", art, "-map", "0:a", "-map", "1:v", "-map_metadata", "-1",
                            "-c:a", "copy", "-c:v", "copy", "-id3v2_version", "3",
                            "-metadata:s:v", "title=Album cover",
                            "-metadata:s:v", "comment=Cover (front)", dest], check=True)
    print("readme-music: the library is in", music)


main()
