/* console: which lines the screen shows (console.h, "view.c"). */
#include "console.h"

int64_t view_base(const struct view *v)
{
    /* The current line on the bottom row. */
    int64_t bottom = (int64_t)v->committed + 1 - (int64_t)v->rows;
    if (!v->from_top)
        return bottom;
    /* From the top: the page's first line until the current line passes
     * the bottom row. */
    return (int64_t)v->top > bottom ? (int64_t)v->top : bottom;
}

uint32_t view_back_max(const struct view *v)
{
    int64_t oldest = v->committed > SCROLLBACK ? (int64_t)(v->committed - SCROLLBACK) : 0;
    int64_t base = view_base(v);
    return base > oldest ? (uint32_t)(base - oldest) : 0;
}
