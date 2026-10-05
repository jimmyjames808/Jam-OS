/* libfun: the smooth text's built-in faces (fun.h "smooth text"): Inter
 * Regular and Medium (third_party/inter, SIL Open Font License 1.1, cut
 * to the glyphs libfun draws by tools/subsetfont.py), linked in as they
 * are with the assembler's .incbin: no copy in the source tree, no
 * generated file, and no filesystem needed to draw a title.
 *
 * Its own object in libfun.a, so only a program that opens a font
 * (font_open in font.c, the only caller) links the ~94 KB in. The paths
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

extern const uint8_t inter_regular[], inter_regular_end[];
extern const uint8_t inter_medium[], inter_medium_end[];

const uint8_t *font_face(enum font_weight w, size_t *n)
{
    switch (w) {
    case FONT_REGULAR:
        *n = (size_t)(inter_regular_end - inter_regular);
        return inter_regular;
    case FONT_MEDIUM:
        *n = (size_t)(inter_medium_end - inter_medium);
        return inter_medium;
    }
    return NULL;
}
