/* libfun: the smooth text's built-in faces (fun.h "smooth text"): Inter
 * Regular and Medium (third_party/inter) and the terminal's JetBrains
 * Mono Regular and Bold (third_party/jetbrains-mono), all SIL Open Font
 * License 1.1, cut to the glyphs libfun draws by tools/subsetfont.py,
 * linked in as they are with the assembler's .incbin: no copy in the
 * source tree, no generated file, and no filesystem needed to draw a
 * title.
 *
 * Its own object in libfun.a, so only a program that opens a font
 * (font_open in font.c, the only caller) links the ~160 KB in. The paths
 * are from the repository's root, where make runs the compiler; the
 * Makefile names the files as this object's prerequisites. The same file
 * builds for the Mac's preview (tools/fontpreview.c), whose assembler
 * takes another section name and puts '_' before C names. */
#include "internal.h"

#define STR2(x) #x
#define STR(x)  STR2(x)
/* The assembler's name for C's name n. */
#define ASM_NAME(n) STR(__USER_LABEL_PREFIX__) #n

#ifdef __APPLE__
#define RODATA ".const_data"
#else
#define RODATA ".section .rodata"
#endif

/* Start and end labels around each file's bytes, read-only data. */
#define FACE(name, path)                                       \
    __asm__(RODATA "\n"                                        \
            ".balign 16\n" ASM_NAME(name) ":\n"                \
            ".incbin \"" path "\"\n" ASM_NAME(name##_end) ":\n" \
            ".text\n")

FACE(inter_regular, "third_party/inter/Inter-Regular.ttf");
FACE(inter_medium, "third_party/inter/Inter-Medium.ttf");
FACE(mono_regular, "third_party/jetbrains-mono/JetBrainsMono-Regular.ttf");
FACE(mono_bold, "third_party/jetbrains-mono/JetBrainsMono-Bold.ttf");

extern const uint8_t inter_regular[], inter_regular_end[];
extern const uint8_t inter_medium[], inter_medium_end[];
extern const uint8_t mono_regular[], mono_regular_end[];
extern const uint8_t mono_bold[], mono_bold_end[];

const uint8_t *font_face(enum font_weight w, size_t *n, int *slots)
{
    const uint8_t *start, *end;
    switch (w) {
    case FONT_REGULAR:
        start = inter_regular, end = inter_regular_end;
        break;
    case FONT_MEDIUM:
        start = inter_medium, end = inter_medium_end;
        break;
    case FONT_MONO:
        start = mono_regular, end = mono_regular_end;
        break;
    case FONT_MONO_BOLD:
        start = mono_bold, end = mono_bold_end;
        break;
    default:
        return NULL;
    }
    *n = (size_t)(end - start);
    *slots = w == FONT_MONO || w == FONT_MONO_BOLD ? FONT_SLOTS : FONT_TEXT_SLOTS;
    return start;
}
