/* fractal: what the iteration kernels (iter.c), the view and its render
 * (render.c), exploring it (main.c) and the self-test (selftest.c) share.
 *
 * Three ways to iterate z -> z^2 + c, picked per view:
 *   - double: the plain loop (and a 4-lane AVX2 version of exactly the
 *     same arithmetic when the CPU has it). Good to a zoom of ~1e12: past
 *     that, neighbouring pixels' coordinates differ in the last bits of a
 *     double and the picture falls apart into blocks.
 *   - perturbation (deep Mandelbrot): one reference orbit, the view's
 *     centre, iterated in double-double (two doubles, ~106 bits, ~32
 *     digits); each pixel iterates only its difference from it, dz, in
 *     plain doubles: dz' = 2 Z dz + dz^2 + dc, where dc (the pixel's
 *     offset from the centre) is tiny but exact. When z = Z + dz gets
 *     smaller than dz, or the reference ends (it escaped, or ran out of
 *     iterations), the pixel "rebases": dz = z and it follows the
 *     reference from its start again (Zhuoran's method), so there are no
 *     glitches to detect and fix. The limit is double-double's precision
 *     for the centre and the reference: ~1e-31 relative, a zoom of ~1e28.
 *   - double-double throughout (deep Julia sets): every pixel iterated in
 *     double-double. ~10x slower than double, same limit.
 * The self-test checks each against another where both are valid. */
#pragma once
#include <fun.h>

/* ---- double-double: hi + lo, |lo| <= ulp(hi) / 2 ----------------------------------- */

typedef struct { double hi, lo; } dd;

static inline dd dd_d(double x) { return (dd){ x, 0 }; }

static inline dd two_sum(double a, double b)
{
    double s = a + b, bb = s - a;
    return (dd){ s, (a - (s - bb)) + (b - bb) };
}

static inline dd quick_two_sum(double a, double b)   /* |a| >= |b| */
{
    double s = a + b;
    return (dd){ s, b - (s - a) };
}

/* a * b exactly, as hi + lo (Dekker: split each into 26-bit halves). */
static inline dd two_prod(double a, double b)
{
    const double split = 134217729.0;   /* 2^27 + 1 */
    double t = split * a, ah = t - (t - a), al = a - ah;
    t = split * b;
    double bh = t - (t - b), bl = b - bh, p = a * b;
    return (dd){ p, ((ah * bh - p) + ah * bl + al * bh) + al * bl };
}

static inline dd dd_add(dd a, dd b)
{
    dd s = two_sum(a.hi, b.hi), t = two_sum(a.lo, b.lo);
    s.lo += t.hi;
    s = quick_two_sum(s.hi, s.lo);
    s.lo += t.lo;
    return quick_two_sum(s.hi, s.lo);
}

static inline dd dd_neg(dd a) { return (dd){ -a.hi, -a.lo }; }
static inline dd dd_sub(dd a, dd b) { return dd_add(a, dd_neg(b)); }

static inline dd dd_add_d(dd a, double b)
{
    dd s = two_sum(a.hi, b);
    s.lo += a.lo;
    return quick_two_sum(s.hi, s.lo);
}

static inline dd dd_mul(dd a, dd b)
{
    dd p = two_prod(a.hi, b.hi);
    p.lo += a.hi * b.lo + a.lo * b.hi;
    return quick_two_sum(p.hi, p.lo);
}

static inline dd dd_mul_d(dd a, double b)
{
    dd p = two_prod(a.hi, b);
    p.lo += a.lo * b;
    return quick_two_sum(p.hi, p.lo);
}

static inline dd dd_sqr(dd a)
{
    dd p = two_prod(a.hi, a.hi);
    p.lo += 2 * a.hi * a.lo;
    return quick_two_sum(p.hi, p.lo);
}

/* a / b to double-double accuracy (two long-division steps). */
static inline dd dd_div(dd a, dd b)
{
    double q1 = a.hi / b.hi;
    dd r = dd_sub(a, dd_mul_d(b, q1));
    double q2 = r.hi / b.hi;
    r = dd_sub(r, dd_mul_d(b, q2));
    double q3 = r.hi / b.hi;
    return dd_add_d(quick_two_sum(q1, q2), q3);
}

/* "-0.77568376800905379746948350393" with `digits` decimals. */
char *dd_str(char *buf, size_t size, dd x, int digits);

/* ---- the kernels ----------------------------------------------------------------------- */

enum { M_DOUBLE, M_PERTURB, M_DD };

/* Everything a kernel needs to know about the view. */
struct kview {
    int    mode;          /* M_* */
    bool   julia;         /* the Julia set of (jr, ji), not the Mandelbrot set */
    dd     cx, cy;        /* the centre */
    double px;            /* one pixel, in the plane */
    double jr, ji;        /* the Julia constant */
    int    maxit;         /* iterations before a point counts as inside */
    bool   avx2;          /* use the 4-lane kernel for M_DOUBLE */
};

/* The reference orbit for M_PERTURB: Z_0 .. Z_n (doubles, rounded from
 * double-double) of the view's centre. */
struct ref {
    double *zr, *zi;      /* Z_i, real and imaginary parts (malloc'd) */
    int n, cap;           /* entries; room */
};
/* (Re)compute it for v (single thread; ~maxit double-double steps). */
bool ref_build(struct ref *r, const struct kview *v);

/* The smooth iteration count at each of n points, given as offsets from
 * the centre in pixels (ox, oy); -1 for points that never escape. Adds the
 * iterations done to *iters. */
void eval_points(const struct kview *v, const struct ref *r, int n, const double *ox,
                 const double *oy, float *out, uint64_t *iters);

/* The single-point kernels (for the self-test). */
float it_double(double cr, double ci, bool julia, double jr, double ji, int maxit,
                uint64_t *iters);
void  it_double4(const double *cr, const double *ci, bool julia, double jr, double ji, int maxit,
                 float *out, uint64_t *iters);   /* AVX2: only if fun_has_avx2() */
float it_dd(dd cr, dd ci, bool julia, double jr, double ji, int maxit, uint64_t *iters);
float it_perturb(const struct ref *r, double dcr, double dci, int maxit, uint64_t *iters);

/* ---- the view and its render (render.c) ---------------------------------------------------- */

#define TS        16             /* tile size */
#define MAXSS     4              /* anti-aliasing: up to MAXSS x MAXSS samples */
#define DEEP_ZOOM 1e12           /* past this: perturbation or double-double */
#define MAX_ZOOM  1e28

struct view {
    bool   julia;                /* the Julia set, not the Mandelbrot set */
    dd     cx, cy;               /* the centre */
    double zoom;                 /* 1: the whole set */
    double jr, ji;               /* the Julia constant */
    int    maxit;                /* 0: automatic */
    int    ss;                   /* samples per pixel on edges: ss x ss (1: off) */
};
extern struct view view;         /* where we are */
extern struct kview kv;          /* what the kernels see of it (apply_view) */
extern struct ref ref;
extern uint64_t iters_by[FUN_MAX_THREADS];   /* iterations per pool thread (work, benchmark) */

extern int PW, PH, NT;           /* the picture: pixels, and tiles of TS x TS */
extern float *nu;                /* per pixel: smooth count, < 0 inside, -2 unknown */
extern uint32_t aa_used;         /* anti-aliased pixels (blocks of samples) */
/* The level being computed: 16..1, 0 anti-aliasing, -1 done. */
extern int pass_target;
extern uint64_t view_t0, view_ns, view_iters;   /* this view's start, time and iterations */

/* Colours: the palette (pal_names[pal_kind]), the colour density
 * (dens_v[dens_i]), the colour cycling shift, and the smallest count in
 * view (nu_min) and the one the colours are measured from (nu_min_shown). */
#define NPALS 8
extern const char *const pal_names[NPALS];
extern int pal_kind, dens_i;
extern const float dens_v[4];
extern double pal_shift;
extern float nu_min, nu_min_shown;
void make_palette(int kind);
/* Colour every tile (all) or the ones marked, into scr.s. */
void colour_tiles(bool all);
/* Mark the tiles under a rectangle to be coloured again. */
void dirty_rect(int x, int y, int w, int h);

/* Allocate the picture for w x h pixels; false: out of memory. */
bool view_alloc(int w, int h);
/* The view is past DEEP_ZOOM. */
bool deep(void);
/* kv from `view` (and the reference orbit, deep). */
void apply_view(void);
/* Start the view again (a new place, a new kind of view); keep_picture:
 * show the old picture until the passes replace it (same place). */
void reset_tiles(bool keep_picture);
/* The view was zoomed by f about its centre: the old picture stretched is
 * the first guess. */
void reproject(double f);
/* The view moved by (dx, dy) pixels (whole tiles): keep what is still on screen. */
void shift(int dx, int dy);
/* a: the next anti-aliasing setting (off, 2x2, 3x3, 4x4). */
void aa_next(void);
/* Work on the passes until `until` (ns; at least one slice) or until
 * everything is done. */
void work(uint64_t until);
/* How far the passes are, 0..1. */
double progress(void);
/* The whole view, all passes, on the pool (or one thread); the iterations. */
uint64_t render_all(bool parallel);

/* ---- main.c, selftest.c ---- */

/* The tour's target, a Misiurewicz point (main.c). */
extern const dd tour_x, tour_y;
int fractal_selftest(void);
