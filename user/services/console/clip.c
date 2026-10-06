/* console: copy and paste in a terminal window (console.h, "clip.c"): the
 * mouse's selection on the text, and the keys that copy it and paste.
 *
 * The mouse. While the program with the keys hasn't asked for the mouse
 * (keys.c: a shell, a command) and the text screen is up (not a full-screen
 * program's alternate screen), the left button selects (select.c has the
 * geometry): a press and a drag select cells, across lines; a double click
 * a word, a triple click a line, and a drag after them goes on by words or
 * lines. Dragging above or below the text scrolls it a line a move, into
 * the scrollback. A click, or a key typed, clears the selection. The wheel
 * still scrolls back (keys.c). A program that asked for the mouse gets it
 * all, as before.
 *
 * The keys (wlinput.c's clip_key_of): Super+C or Ctrl+Shift+C copies the
 * selection to the clipboard (window.c offers it through the compositor);
 * Super+V or Ctrl+Shift+V pastes the clipboard's text (window.c asks,
 * paste.c types it). Neither key, nor its release, reaches the program;
 * Ctrl+C alone is the interrupt as ever. The compositor reserves neither
 * Super key (its own keys are a fixed table), so both reach the focused
 * terminal.
 *
 * The selected cells are drawn on a blackcurrant tint (cells.h's
 * CELL_SELECTED), the text unchanged. */
#include <jwl_client.h>
#include "console.h"

#define BTN_LEFT 0x110   /* evdev's left button */

struct selection sel;
static struct click_track clicks;
static bool dragging;               /* the left button went down on the text */
static int32_t ptr_x, ptr_y;        /* the pointer, surface pixels */
static bool swallow[2];             /* the release of C (0), V (1) is a copy or paste key's */
static bool range_on;               /* clip_begin's: a range to mark */
static struct sel_pt range_from, range_to;

/* ---- the text, as the selection reads it ----------------------------------------- */

static const struct cell *text_line(void *ctx, int64_t i)
{
    (void)ctx;
    int64_t oldest = committed > SCROLLBACK ? (int64_t)(committed - SCROLLBACK) : 0;
    if (i == (int64_t)committed)
        return cur;
    return i >= oldest && i < (int64_t)committed ? line((uint64_t)i) : NULL;
}

static struct sel_text text_now(void)
{
    return (struct sel_text){ text_line, NULL, cols };
}

/* The point of the text under surface pixel (x, y). */
static struct sel_pt point_at(int32_t x, int32_t y)
{
    uint32_t col, row;
    sel_cell_at(&look, cols, rows, x, y, &col, &row);
    struct view v = view_now();
    int64_t i = view_base(&v) - view_back + row;   /* the line on that row (grid_walk's) */
    int64_t oldest = committed > SCROLLBACK ? (int64_t)(committed - SCROLLBACK) : 0;
    i = i < oldest ? oldest : i > (int64_t)committed ? (int64_t)committed : i;
    return (struct sel_pt){ i, col };
}

void clip_clear(void)
{
    if (sel.on)
        dirty = true;
    sel.on = false;
    dragging = false;
}

void clip_begin(void)
{
    struct sel_text t = text_now();
    range_on = window_mode && !alt_on && sel_range(&sel, &t, &range_from, &range_to);
}

bool clip_marked(int64_t line_no, uint32_t col)
{
    return range_on && sel_has(&range_from, &range_to, line_no, col);
}

/* ---- the mouse -------------------------------------------------------------------- */

/* A drag at (x, y): above or below the text it scrolls a line first. */
static void drag_to(int32_t x, int32_t y)
{
    struct view v = view_now();
    uint32_t max = view_back_max(&v);
    if (y < WIN_PAD && view_back < max) {
        view_back++;
        dirty = true;
    } else if (y >= WIN_PAD + (int32_t)rows * look.h && view_back) {
        view_back--;
        dirty = true;
    }
    if (sel_drag(&sel, point_at(x, y)))
        dirty = true;
}

static bool button(const struct jwl_event *ev)
{
    if (ev->button.button != BTN_LEFT)
        return false;
    if (!ev->button.pressed) {
        dragging = false;
        return true;
    }
    struct sel_pt at = point_at(ptr_x, ptr_y);
    unsigned n = sel_click(&clicks, ev->button.time, at);
    if (sel.on || n > 1)
        dirty = true;
    sel_press(&sel, at, n);
    dragging = true;
    return true;
}

bool clip_pointer(const struct jwl_event *ev)
{
    if (ev->type == JWL_EV_POINTER_ENTER || ev->type == JWL_EV_POINTER_MOTION) {
        ptr_x = ev->pointer.x >> 8;
        ptr_y = ev->pointer.y >> 8;
    }
    if (alt_on || focus_wants_mouse()) {
        dragging = false;
        return false;   /* the program's */
    }
    if (ev->type == JWL_EV_POINTER_BUTTON)
        return button(ev);
    if (ev->type == JWL_EV_POINTER_MOTION && dragging) {
        drag_to(ptr_x, ptr_y);
        return true;
    }
    return false;
}

/* ---- the keys --------------------------------------------------------------------- */

/* The selection to the clipboard. */
static void copy(void)
{
    struct sel_pt a, b;
    struct sel_text t = text_now();
    if (!sel_range(&sel, &t, &a, &b))
        return;   /* nothing selected: the clipboard keeps what it has */
    /* at most 3 bytes a cell and a newline a line, and the clipboard's most */
    uint64_t want = (uint64_t)(b.line - a.line + 1) * (cols * 3u + 1u);
    size_t cap = want < JWL_CLIP_MAX ? (size_t)want : JWL_CLIP_MAX;
    char *text = malloc(cap);
    if (!text) {
        printf("console: terminal %u: no memory to copy the selection\n", term_no);
        return;
    }
    size_t n = sel_copy(&sel, &t, text, cap);
    if (window_copy(text, n))
        printf("console: terminal %u: copied %lu bytes\n", term_no, (unsigned long)n);
    free(text);
}

bool clip_key(const struct input_key_event *ev)
{
    unsigned which = ev->usage == 0x19;   /* V: 1, C (and the rest): 0 */
    bool cv = ev->usage == 0x06 || ev->usage == 0x19;
    if (ev->state == INPUT_KEY_UP && cv && swallow[which]) {
        swallow[which] = false;   /* the chord's release: no further */
        return true;
    }
    enum clip_key k = ev->state == INPUT_KEY_UP ? CLIP_NONE : clip_key_of(ev);
    if (k == CLIP_NONE) {
        if (key_clears_selection(ev)) {
            clip_clear();
            paste_stop();   /* typing ends a long paste still going */
        }
        return false;
    }
    swallow[which] = true;
    if (ev->state == INPUT_KEY_REPEAT)
        return true;   /* held down: once */
    if (k == CLIP_COPY)
        copy();
    else
        window_paste();
    return true;
}
