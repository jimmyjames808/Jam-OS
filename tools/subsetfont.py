#!/usr/bin/env python3
"""Cut an upstream TTF down to what libfun's smooth text draws.

    python3 tools/subsetfont.py <upstream.ttf> <out.ttf>              Inter's
    python3 tools/subsetfont.py --terminal <upstream.ttf> <out.ttf>   JetBrains Mono's

Run on the Mac (fontTools: `pip3 install fonttools`); the output is
committed in third_party/inter/ and its command lines are in
third_party/VERSIONS.md (Inter's in third_party/inter/, the terminal's
JetBrains Mono in third_party/jetbrains-mono/). What it does, in order:

1. Subsets the font to the code points libfun has slots for
   (user/apps/fun/font.c, `font_slot`): printable ASCII, U+00A0..U+00FF and
   a few punctuation marks titles often hold, plus .notdef (the box drawn
   for any other code point); with --terminal (the console's faces) also
   Latin Extended-A, U+0100..U+017F (the console's 8x16 font has them
   too), and the box drawing and block elements, U+2500..U+259F, so
   text-mode art lines up. Hinting is dropped (stb_truetype doesn't
   run it), and so are the layout tables after step 2.
2. Flattens the kerning. Inter kerns through GPOS: class-based pair
   lookups, one of them wrapped in an extension lookup, which
   stb_truetype's GPOS reader skips (it reads plain pair lookups only, and
   stops at the first one that matches). Every pair of kept glyphs is
   evaluated here as a shaper would for Latin text: each lookup of the
   'latn' script's 'kern' feature in lookup order, the first subtable that
   applies, the first glyph's x advance summed. The non-zero pairs go into
   a legacy 'kern' table (version 0, format 0), which stb_truetype reads
   whole (stbtt_GetKerningTable); GPOS, GSUB and GDEF are then dropped.
   Text is drawn without shaping, so nothing else in them is used. A
   font with no kerning (a monospace face) gets no 'kern' table.

Deterministic: the same input gives the same bytes (the subsetter's
timestamps are left as upstream's).
"""
import sys

from fontTools import subset
from fontTools.ttLib import TTFont, newTable
from fontTools.ttLib.tables._k_e_r_n import KernTable_format_0

# The code points kept: printable ASCII, Latin-1's printable half, and
# the punctuation in EXTRA. user/apps/fun/font.c's slot table must match.
EXTRA = [0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x20AC]
UNICODES = list(range(0x20, 0x7F)) + list(range(0xA0, 0x100)) + EXTRA
# --terminal: Latin Extended-A, then the box drawing and block elements
# (font.c's slots after .notdef's, in this order).
TERMINAL = list(range(0x100, 0x180)) + list(range(0x2500, 0x25A0))

# A 'kern' format 0 subtable holds at most this many pairs (its length
# field is 16 bits: 14 header bytes + 6 a pair).
KERN0_MAX_PAIRS = (0xFFFF - 14) // 6


def subset_font(path, unicodes):
    opts = subset.Options()
    opts.hinting = False
    opts.layout_features = ["kern"]
    opts.notdef_outline = True
    opts.name_IDs = ["*"]
    opts.name_languages = ["*"]
    opts.drop_tables += ["DSIG"]
    font = TTFont(path, recalcTimestamp=False)
    sub = subset.Subsetter(opts)
    sub.populate(unicodes=unicodes)
    sub.subset(font)
    return font


def unwrap(lookup):
    """A lookup's subtables, extension wrappers removed."""
    out = []
    for st in lookup.SubTable:
        out.append(st.ExtSubTable if lookup.LookupType == 9 else st)
    return out


def kern_lookups(gpos):
    """The lookups the 'latn' script's default 'kern' feature uses, in
    lookup-list order (the order a shaper applies them in)."""
    table = gpos.table
    want = set()
    for sr in table.ScriptList.ScriptRecord:
        if sr.ScriptTag != "latn":
            continue
        for fi in sr.Script.DefaultLangSys.FeatureIndex:
            fr = table.FeatureList.FeatureRecord[fi]
            if fr.FeatureTag == "kern":
                want.update(fr.Feature.LookupListIndex)
    return [table.LookupList.Lookup[i] for i in sorted(want)]


def subtable_value(st, g1, g2):
    """(applies, x advance) of one PairPos subtable for the pair."""
    if g1 not in st.Coverage.glyphs:
        return False, 0
    if st.Format == 1:
        ps = st.PairSet[st.Coverage.glyphs.index(g1)]
        for rec in ps.PairValueRecord:
            if rec.SecondGlyph == g2:
                v = rec.Value1
                return True, getattr(v, "XAdvance", 0) if v else 0
        return False, 0
    c1 = st.ClassDef1.classDefs.get(g1, 0)
    c2 = st.ClassDef2.classDefs.get(g2, 0)
    v = st.Class1Record[c1].Class2Record[c2].Value1
    return True, getattr(v, "XAdvance", 0) if v else 0


def pair_value(lookups, g1, g2):
    total = 0
    for lookup in lookups:
        for st in lookup:
            applies, adv = subtable_value(st, g1, g2)
            if applies:
                total += adv
                break
    return total


def flatten_kerning(font):
    if "GPOS" not in font:
        return {}
    lookups = [unwrap(lk) for lk in kern_lookups(font["GPOS"])]
    for lookup in lookups:
        for st in lookup:
            if st.ValueFormat1 & ~4 or st.ValueFormat2:
                sys.exit("subsetfont: a kern value other than the first glyph's x advance")
    # Only glyphs a code point maps to: the rest are pieces of composite
    # glyphs (accents), never drawn on their own.
    cmap = font.getBestCmap()
    glyphs = sorted(set(cmap.values()), key=font.getGlyphID)
    pairs = {}
    for g1 in glyphs:
        for g2 in glyphs:
            v = pair_value(lookups, g1, g2)
            if v:
                pairs[(g1, g2)] = v
    return pairs


def main():
    args = sys.argv[1:]
    terminal = args[:1] == ["--terminal"]
    if terminal:
        args = args[1:]
    if len(args) != 2:
        sys.exit("\n".join(l.strip() for l in __doc__.strip().splitlines()[2:4]))
    font = subset_font(args[0], UNICODES + (TERMINAL if terminal else []))
    pairs = flatten_kerning(font)
    if len(pairs) > KERN0_MAX_PAIRS:
        sys.exit(f"subsetfont: {len(pairs)} kerning pairs, a 'kern' subtable holds {KERN0_MAX_PAIRS}")
    for tag in ("GPOS", "GSUB", "GDEF"):
        if tag in font:
            del font[tag]
    if pairs:
        add_kern(font, pairs)
    font.save(args[1])
    print(f"{args[1]}: {len(font.getGlyphOrder())} glyphs, {len(pairs)} kerning pairs")


def add_kern(font, pairs):
    """pairs as a legacy 'kern' table (version 0, one format 0 subtable)."""
    kern = newTable("kern")
    kern.version = 0
    sub = KernTable_format_0()
    sub.version = 0
    sub.format = 0
    sub.coverage = 1   # horizontal kerning
    sub.tupleIndex = None
    sub.kernTable = pairs
    kern.kernTables = [sub]
    font["kern"] = kern


if __name__ == "__main__":
    main()
