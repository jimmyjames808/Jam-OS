/* sysmon: the picture (sysmon.h). layout.c says where things go; this
 * draws them: what never changes (the backdrop, the cards and tiles, the
 * headings) once into `bg`, and each frame the numbers, bars and graphs
 * over a copy of it.
 *
 * P threads and E threads differ in colour everywhere they show (tile
 * edge, badge, bar, graph): blue for P, orange for E, violet for a CPU
 * that says neither (QEMU's). A graph is the last readings as columns,
 * the newest at the right. */
#include "sysmon.h"

#define INK_DIM   0x6b7bb0   /* headings */
#define INK_SOFT  0x8fa3d8   /* labels */
#define INK       0xe0e6ff   /* text */
#define WELL      0x05060c   /* a bar's or graph's empty part */

static struct layout lay;
static struct surf bg;
static int u;

static uint32_t type_rgb(uint32_t type)
{
    return type == CPU_TYPE_PERFORMANCE ? 0x4a9cff : type == CPU_TYPE_EFFICIENCY ? 0xffa23c
                                                                                   : 0xa08cff;
}

static const char *type_letter(uint32_t type)
{
    return type == CPU_TYPE_PERFORMANCE ? "P" : type == CPU_TYPE_EFFICIENCY ? "E" : "";
}

/* A load's colour: calm, busy, flat out. */
static uint32_t load_rgb(uint32_t pm)
{
    return pm >= 900 ? 0xff6a5a : pm >= 600 ? 0xffd24a : 0x7fe08a;
}

/* ---- the background ---- */

static const char *const card_name[NCARDS] = { "CPU", "MEMORY", "CONTEXT SWITCHES", "UPTIME" };
static const char *const group_name[NGROUPS] = { "P-CORES", "E-CORES", "CPUS" };

/* The process table's columns: where each starts, in 1/1000 of its width. */
static const struct column {
    const char *name;
    int         at;      /* permille of the table's inner width */
    bool        right;   /* right-aligned, ending at the next column's start */
} columns[] = {
    { "PID", 0, false }, { "NAME", 90, false }, { "CPU %", 420, true }, { "CPU TIME", 580, true },
    { "THREADS", 740, true }, { "MEMORY", 860, true },
};
#define NCOLS ((int)(sizeof(columns) / sizeof(columns[0])))

/* Column c's text box on the row at y. */
static struct rect column_box(int c, int y)
{
    int x0 = lay.table.x + 12 * u, w = lay.table.w - 24 * u;
    int from = x0 + w * columns[c].at / 1000;
    int to = x0 + (c + 1 < NCOLS ? w * columns[c + 1].at / 1000 : w);
    return (struct rect){ from, y, to - from - 10 * u, lay.row_h };
}

static void column_text(const struct surf *s, int c, int y, uint32_t rgb, const char *str)
{
    struct rect r = column_box(c, y);
    int x = columns[c].right ? r.x + r.w - text_width(u, str) : r.x;
    text(s, x, y + (lay.row_h - TEXT_H(u)) / 2, u, rgb, str);
}

static void draw_background(const struct model *m)
{
    struct surf *s = &bg;
    char a[96];
    vgrad(s, &(struct rect){ 0, 0, s->w, s->h }, 0x182044, 0x05060c);
    text_shadow(s, lay.title.x, lay.title.y, 2 * u, INK, "SYSTEM MONITOR");
    snprintf(a, sizeof(a), "Jam OS %s    %s    q: quit", m->version, m->brand);
    text(s, lay.title.x + lay.title.w - text_width(u, a), lay.title.y + 8 * u, u, INK_DIM, a);
    for (int i = 0; i < NCARDS; i++) {
        card(s, &lay.card[i], 8 * u);
        text(s, lay.card[i].x + 12 * u, lay.card[i].y + 8 * u, u, INK_SOFT, card_name[i]);
    }
    for (int g = 0; g < NGROUPS; g++) {
        if (!lay.label[g].w)
            continue;
        snprintf(a, sizeof(a), "%s  %u threads", group_name[g], m->group[g]);
        text(s, lay.label[g].x + 2 * u, lay.label[g].y, u, g == 2 ? INK_SOFT : type_rgb(g + 1), a);
    }
    for (uint32_t k = 0; k < m->ncpu; k++) {
        const struct rect *r = &lay.cpu[k];
        panel(s, &(struct rect){ r->x - 1, r->y - 1, r->w + 2, r->h + 2 }, 6 * u + 1,
              type_rgb(m->cpu[k].type), 90);
        panel(s, r, 6 * u, 0x0c1022, 256);
    }
    card(s, &lay.table, 8 * u);
    for (int c = 0; c < NCOLS; c++)
        column_text(s, c, lay.table.y + 8 * u, INK_SOFT, columns[c].name);
}

bool draw_setup(const struct model *m)
{
    u = scr.ui;
    layout_make(&lay, scr.w, scr.h, u, m->group);
    bg = surf_new(scr.w, scr.h);
    if (!bg.px)
        return false;
    draw_background(m);
    return true;
}

/* ---- bars and graphs ---- */

/* History h as columns in r, the newest at the right, full height = max. */
static void graph(const struct rect *r, const struct history *h, uint32_t max, uint32_t rgb)
{
    const struct surf *s = &scr.s;
    int step = 2 * u;
    fill_rect(s, r, WELL);
    max = max ? max : 1;
    for (int k = 0; k < h->n && (k + 1) * step <= r->w; k++) {
        uint32_t v = hist_back(h, k) > max ? max : hist_back(h, k);
        int ch = (int)((uint64_t)v * (uint64_t)r->h / max), x = r->x + r->w - (k + 1) * step;
        fill(s, x, r->y + r->h - ch, step, ch, mixc(rgb, WELL, 150));
        fill(s, x, r->y + r->h - ch - (ch ? 0 : u), step, u, rgb);   /* the line on top */
    }
}

/* A horizontal bar: part / whole of r filled. */
static void hbar(const struct rect *r, uint64_t part, uint64_t whole, uint32_t rgb)
{
    int fw = whole ? (int)(part * (uint64_t)r->w / whole) : 0;
    panel(&scr.s, r, r->h / 2, WELL, 256);
    if (part && fw < r->h)
        fw = r->h;   /* anything at all shows as a dot */
    if (fw)
        panel(&scr.s, &(struct rect){ r->x, r->y, fw > r->w ? r->w : fw, r->h }, r->h / 2, rgb,
              256);
}

static void percent(char *buf, size_t cap, uint32_t pm)
{
    snprintf(buf, cap, "%u%%", (pm + 5) / 10);
}

/* ---- the cards ---- */

/* A card's big figure and the small line under it. */
static void card_figure(const struct rect *r, const char *big, uint32_t rgb, const char *small)
{
    text_shadow(&scr.s, r->x + 12 * u, r->y + 28 * u, 2 * u, rgb, big);
    text(&scr.s, r->x + 12 * u, r->y + 68 * u, u, INK_DIM, small);
}

/* The right half of a card, for its graph. */
static struct rect card_graph(const struct rect *r)
{
    return (struct rect){ r->x + r->w / 2, r->y + 12 * u, r->w / 2 - 12 * u, r->h - 24 * u };
}

static void draw_cards(const struct model *m)
{
    char a[64], b[64], n1[24], n2[24];
    struct rect g;

    percent(a, sizeof(a), m->total_pm);
    if (m->group[0] || m->group[1])
        snprintf(b, sizeof(b), "%u CPUs: %u P + %u E", m->ncpu, m->group[0], m->group[1]);
    else
        snprintf(b, sizeof(b), "%u CPUs", m->ncpu);
    card_figure(&lay.card[0], a, load_rgb(m->total_pm), b);
    g = card_graph(&lay.card[0]);
    graph(&g, &m->total, 1000, 0x5ad0ff);

    snprintf(b, sizeof(b), "of %s, %s free", fmt_bytes(n1, sizeof(n1), m->mem_total),
             fmt_bytes(n2, sizeof(n2), m->mem_total - m->mem_used));
    card_figure(&lay.card[1], fmt_bytes(a, sizeof(a), m->mem_used), 0xffe07a, b);
    g = card_graph(&lay.card[1]);
    g.y += g.h / 2 - 6 * u;
    g.h = 12 * u;
    hbar(&g, m->mem_used, m->mem_total, 0xffe07a);

    snprintf(a, sizeof(a), "%s /s", fmt_count(n1, sizeof(n1), m->switches_s));
    snprintf(b, sizeof(b), "peak %s /s", fmt_count(n2, sizeof(n2), hist_max(&m->switches)));
    card_figure(&lay.card[2], a, 0xc8a0ff, b);
    g = card_graph(&lay.card[2]);
    graph(&g, &m->switches, hist_max(&m->switches), 0xc8a0ff);

    snprintf(b, sizeof(b), "%u processes, %u threads", m->nproc, m->nthreads);
    card_figure(&lay.card[3], fmt_uptime(a, sizeof(a), m->uptime_ns), 0xffffff, b);
}

/* ---- the CPU tiles ---- */

static void draw_tile(const struct cpu_view *c, const struct rect *r)
{
    const struct surf *s = &scr.s;
    char a[16];
    uint32_t rgb = type_rgb(c->type);
    int in = 8 * u, ty = r->y + 5 * u;
    snprintf(a, sizeof(a), "%u", c->index);
    int x = text(s, r->x + in, ty, u, INK, a);
    text(s, x + 5 * u, ty, u, rgb, type_letter(c->type));
    percent(a, sizeof(a), c->pm);
    text(s, r->x + r->w - in - text_width(u, a), ty, u, load_rgb(c->pm), a);
    /* Under the text: the bar (now) and the graph (before). */
    int top = ty + TEXT_H(u) + 3 * u, gh = r->y + r->h - 6 * u - top;
    struct rect bar = { r->x + in, top, 8 * u, gh };
    fill_rect(s, &bar, WELL);
    int bh = (int)(c->pm * (uint32_t)gh / 1000);
    fill(s, bar.x, bar.y + gh - bh, bar.w, bh, rgb);
    struct rect g = { bar.x + bar.w + 5 * u, top, r->x + r->w - in - (bar.x + bar.w + 5 * u), gh };
    graph(&g, &c->h, 1000, rgb);
}

/* ---- the processes ---- */

static void draw_procs(const struct model *m)
{
    const struct surf *s = &scr.s;
    char a[32];
    int y = lay.table.y + 8 * u + lay.row_h;
    for (int i = 0; i < m->ntop && i < lay.rows; i++, y += lay.row_h) {
        const struct proc_view *p = &m->top[i];
        if (i % 2 == 0)
            blend(s, &(struct rect){ lay.table.x + 6 * u, y, lay.table.w - 12 * u, lay.row_h },
                  0x3b4c86, 40);
        /* The CPU column's box fills with the process's share of one CPU. */
        struct rect box = column_box(2, y);
        int bw = (int)((p->pm > 1000 ? 1000 : p->pm) * (uint32_t)box.w / 1000);
        blend(s, &(struct rect){ box.x + box.w - bw, y + 3 * u, bw, lay.row_h - 6 * u },
              load_rgb(p->pm), 70);
        snprintf(a, sizeof(a), "%lu", (unsigned long)p->koid);
        column_text(s, 0, y, INK_SOFT, a);
        column_text(s, 1, y, INK, p->name);
        snprintf(a, sizeof(a), "%u.%u", p->pm / 10, p->pm % 10);
        column_text(s, 2, y, p->pm ? 0xffffff : INK_SOFT, a);
        column_text(s, 3, y, INK, fmt_cpu_time(a, sizeof(a), p->cpu_ns));
        snprintf(a, sizeof(a), "%u", p->threads);
        column_text(s, 4, y, INK, a);
        column_text(s, 5, y, INK, fmt_bytes(a, sizeof(a), p->mem));
    }
}

void draw(const struct model *m)
{
    blit(&scr.s, 0, 0, &bg, &(struct rect){ 0, 0, scr.w, scr.h });
    draw_cards(m);
    for (uint32_t k = 0; k < m->ncpu; k++)
        draw_tile(&m->cpu[k], &lay.cpu[k]);
    draw_procs(m);
}
