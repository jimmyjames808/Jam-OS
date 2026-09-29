/* fractal: the iteration kernels (see fractal.h). */
#include "fractal.h"

#define BAILOUT 256.0

static inline float smooth(int i, double r2)
{
    /* i iterations done, |z|^2 = r2 >= BAILOUT: the continuous count */
    double nu = i + 1 - log2d(log2d(r2) * 0.5);
    return nu > 0 ? (float)nu : 0.0f;
}

/* ---- double -------------------------------------------------------------------------- */

static inline bool in_cardioid(double pr, double pi)
{
    /* The main cardioid and the period-2 bulb never escape. */
    double q = (pr - 0.25) * (pr - 0.25) + pi * pi;
    return q * (q + (pr - 0.25)) < 0.25 * pi * pi || (pr + 1) * (pr + 1) + pi * pi < 0.0625;
}

float it_double(double pr, double pi, bool julia, double jr, double ji, int maxit, uint64_t *iters)
{
    double zr, zi, kr, ki;
    if (julia) {
        zr = pr, zi = pi, kr = jr, ki = ji;
    } else {
        zr = 0, zi = 0, kr = pr, ki = pi;
        if (in_cardioid(pr, pi))
            return -1;
    }
    double r2 = zr * zr, i2 = zi * zi, sr = zr, si = zi;
    int i = 0, check = 8;
    while (i < maxit && r2 + i2 < BAILOUT) {
        zi = 2 * zr * zi + ki;
        zr = r2 - i2 + kr;
        r2 = zr * zr;
        i2 = zi * zi;
        i++;
        if (i == check) {   /* a cycle (checked at doubling steps): inside */
            if ((zr - sr) * (zr - sr) + (zi - si) * (zi - si) < 1e-28) {
                *iters += (uint64_t)i;
                return -1;
            }
            sr = zr;
            si = zi;
            check *= 2;
        }
    }
    *iters += (uint64_t)i;
    if (i >= maxit)
        return -1;
    return smooth(i, r2 + i2);
}

/* The same arithmetic, in the same order, four points at a time (so it
 * gives exactly it_double's answers: no fused multiply-adds here). */
typedef double v4d __attribute__((vector_size(32)));
typedef long long v4l __attribute__((vector_size(32)));

__attribute__((target("avx2"), optimize("fp-contract=off")))
void it_double4(const double *pr, const double *pi, bool julia, double jr, double ji, int maxit,
                float *out, uint64_t *iters)
{
    v4d zr, zi, kr, ki;
    v4l live = { -1, -1, -1, -1 }, inside = { 0, 0, 0, 0 }, cnt = { 0, 0, 0, 0 };
    v4d cr = { pr[0], pr[1], pr[2], pr[3] }, ci = { pi[0], pi[1], pi[2], pi[3] };
    if (julia) {
        zr = cr;
        zi = ci;
        kr = (v4d){ jr, jr, jr, jr };
        ki = (v4d){ ji, ji, ji, ji };
    } else {
        zr = (v4d){ 0, 0, 0, 0 };
        zi = zr;
        kr = cr;
        ki = ci;
        for (int l = 0; l < 4; l++)
            if (in_cardioid(pr[l], pi[l])) {
                live[l] = 0;
                inside[l] = -1;
            }
    }
    v4d r2 = zr * zr, i2 = zi * zi, sr = zr, si = zi, mag = r2 + i2;
    v4d bail = { BAILOUT, BAILOUT, BAILOUT, BAILOUT }, eps = { 1e-28, 1e-28, 1e-28, 1e-28 };
    live &= (v4l)(mag < bail);
    int check = 8;
    for (int i = 0; i < maxit && (live[0] | live[1] | live[2] | live[3]); i++) {
        zi = 2 * zr * zi + ki;
        zr = r2 - i2 + kr;
        r2 = zr * zr;
        i2 = zi * zi;
        cnt -= live;   /* +1 for every point still going */
        v4d m = r2 + i2;
        /* |z|^2 where a point escapes now; the rest keep their last value */
        v4l esc = live & (v4l)(m >= bail);
        mag = (v4d)(((v4l)m & esc) | ((v4l)mag & ~esc));
        live &= ~esc;
        if (i + 1 == check) {
            v4d dr = zr - sr, di = zi - si;
            v4l cyc = live & (v4l)(dr * dr + di * di < eps);
            inside |= cyc;
            live &= ~cyc;
            sr = zr;
            si = zi;
            check *= 2;
        }
    }
    for (int l = 0; l < 4; l++) {
        *iters += (uint64_t)cnt[l];
        out[l] = inside[l] || live[l] || cnt[l] >= maxit ? -1.0f : smooth((int)cnt[l], mag[l]);
    }
}

/* ---- double-double ------------------------------------------------------------------- */

float it_dd(dd cr, dd ci, bool julia, double jr, double ji, int maxit, uint64_t *iters)
{
    dd zr, zi, kr, ki;
    if (julia) {
        zr = cr, zi = ci, kr = dd_d(jr), ki = dd_d(ji);
    } else {
        zr = dd_d(0), zi = dd_d(0), kr = cr, ki = ci;
    }
    dd r2 = dd_sqr(zr), i2 = dd_sqr(zi);
    int i = 0;
    while (i < maxit && r2.hi + i2.hi < BAILOUT) {
        dd t = dd_mul(zr, zi);
        zi = dd_add((dd){ 2 * t.hi, 2 * t.lo }, ki);
        zr = dd_add(dd_sub(r2, i2), kr);
        r2 = dd_sqr(zr);
        i2 = dd_sqr(zi);
        i++;
    }
    *iters += (uint64_t)i;
    if (i >= maxit)
        return -1;
    return smooth(i, r2.hi + i2.hi);
}

/* ---- perturbation ---------------------------------------------------------------------- */

bool ref_build(struct ref *r, const struct kview *v)
{
    if (r->cap < v->maxit + 1) {
        int cap = v->maxit + 1 > 4096 ? v->maxit + 1 : 4096;
        r->zr = big_alloc((uint64_t)cap * 8);
        r->zi = big_alloc((uint64_t)cap * 8);
        if (!r->zr || !r->zi) {
            r->cap = r->n = 0;
            return false;
        }
        r->cap = cap;
    }
    dd zr = dd_d(0), zi = dd_d(0);
    r->zr[0] = r->zi[0] = 0;
    int m = 0;
    while (m < v->maxit) {
        dd r2 = dd_sqr(zr), i2 = dd_sqr(zi), t = dd_mul(zr, zi);
        zi = dd_add((dd){ 2 * t.hi, 2 * t.lo }, v->cy);
        zr = dd_add(dd_sub(r2, i2), v->cx);
        m++;
        r->zr[m] = zr.hi + zr.lo;
        r->zi[m] = zi.hi + zi.lo;
        if (r->zr[m] * r->zr[m] + r->zi[m] * r->zi[m] > BAILOUT)
            break;   /* escaped: pixels rebase when they get here */
    }
    r->n = m;
    return true;
}

float it_perturb(const struct ref *r, double dcr, double dci, int maxit, uint64_t *iters)
{
    const double *Zr = r->zr, *Zi = r->zi;
    double dzr = 0, dzi = 0, zr = 0, zi = 0, r2 = 0;
    int m = 0, i = 0;
    while (i < maxit) {
        double ar = Zr[m], ai = Zi[m];
        /* dz' = 2 Z dz + dz^2 + dc */
        double nr = 2 * (ar * dzr - ai * dzi) + (dzr * dzr - dzi * dzi) + dcr;
        double ni = 2 * (ar * dzi + ai * dzr) + 2 * dzr * dzi + dci;
        dzr = nr;
        dzi = ni;
        m++;
        i++;
        zr = Zr[m] + dzr;
        zi = Zi[m] + dzi;
        r2 = zr * zr + zi * zi;
        if (r2 >= BAILOUT)
            break;
        if (r2 < dzr * dzr + dzi * dzi || m == r->n) {   /* rebase */
            dzr = zr;
            dzi = zi;
            m = 0;
        }
    }
    *iters += (uint64_t)i;
    if (r2 < BAILOUT || i >= maxit)   /* as it_double: escaping on the last step counts as in */
        return -1;
    return smooth(i, r2);
}

/* ---- many points ---------------------------------------------------------------------- */

void eval_points(const struct kview *v, const struct ref *r, int n, const double *ox,
                 const double *oy, float *out, uint64_t *iters)
{
    if (v->mode == M_PERTURB) {
        for (int i = 0; i < n; i++)
            out[i] = it_perturb(r, ox[i] * v->px, oy[i] * v->px, v->maxit, iters);
        return;
    }
    if (v->mode == M_DD) {
        for (int i = 0; i < n; i++)
            out[i] = it_dd(dd_add_d(v->cx, ox[i] * v->px), dd_add_d(v->cy, oy[i] * v->px),
                           v->julia, v->jr, v->ji, v->maxit, iters);
        return;
    }
    double cx = v->cx.hi + v->cx.lo, cy = v->cy.hi + v->cy.lo;
    int i = 0;
    if (v->avx2) {
        for (; i + 4 <= n; i += 4) {
            double pr[4], pi[4];
            for (int l = 0; l < 4; l++) {
                pr[l] = cx + ox[i + l] * v->px;
                pi[l] = cy + oy[i + l] * v->px;
            }
            it_double4(pr, pi, v->julia, v->jr, v->ji, v->maxit, out + i, iters);
        }
    }
    for (; i < n; i++)
        out[i] = it_double(cx + ox[i] * v->px, cy + oy[i] * v->px, v->julia, v->jr, v->ji,
                           v->maxit, iters);
}

/* ---- printing ---------------------------------------------------------------------------- */

char *dd_str(char *buf, size_t size, dd x, int digits)
{
    size_t o = 0;
    if (size < 4)
        return buf;
    if (x.hi < 0) {
        x = dd_neg(x);
        buf[o++] = '-';
    } else {
        buf[o++] = '+';
    }
    /* round at the last digit printed */
    double half = 0.5;
    for (int i = 0; i < digits; i++)
        half /= 10;
    x = dd_add_d(x, half);
    double w = floord(x.hi);
    x = dd_add_d(x, -w);
    if (x.hi < 0) {
        w -= 1;
        x = dd_add_d(x, 1);
    }
    o += (size_t)snprintf(buf + o, size - o, "%lu.", (unsigned long)w);
    for (int i = 0; i < digits && o + 1 < size; i++) {
        x = dd_mul_d(x, 10);
        double d = floord(x.hi);
        x = dd_add_d(x, -d);
        if (x.hi < 0) {   /* hi rounded up across an integer */
            d -= 1;
            x = dd_add_d(x, 1);
        }
        if (d < 0)
            d = 0;
        if (d > 9)
            d = 9;
        buf[o++] = (char)('0' + (int)d);
    }
    buf[o] = '\0';
    return buf;
}
