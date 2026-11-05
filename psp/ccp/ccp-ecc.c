/*
 * AMD PSP CCP v5 ECC engine arithmetic
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include <gmp.h>

#include "psp/ccp/ccp-ecc.h"

static void slot_get(mpz_t r, const uint8_t *src, unsigned slot)
{
    mpz_import(r, CCP_ECC_WIDTH, -1, 1, 0, 0, src + slot * CCP_ECC_SLOT_SIZE);
}

/* Little-endian, zero padded; the value is below 2^384 as it is reduced */
static void slot_put(uint8_t *dst, const mpz_t v)
{
    size_t count = 0;

    mpz_export(dst, &count, -1, 1, 0, 0, v);
}

typedef struct EccPoint {
    mpz_t x;
    mpz_t y;
    bool inf;
} EccPoint;

typedef struct EccCurve {
    mpz_t p;
    mpz_t a;
} EccCurve;

static void point_init(EccPoint *q)
{
    mpz_init(q->x);
    mpz_init(q->y);
    q->inf = true;
}

static void point_clear(EccPoint *q)
{
    mpz_clear(q->x);
    mpz_clear(q->y);
}

static void point_set(EccPoint *r, const EccPoint *q)
{
    mpz_set(r->x, q->x);
    mpz_set(r->y, q->y);
    r->inf = q->inf;
}

/* r = q + q; false if a needed inverse does not exist */
static bool point_double(const EccCurve *c, EccPoint *r, const EccPoint *q)
{
    mpz_t l, t, x3;
    bool ok = true;

    if (q->inf || mpz_sgn(q->y) == 0) {
        r->inf = true;
        return true;
    }
    mpz_inits(l, t, x3, NULL);
    /* l = (3x^2 + a) / 2y */
    mpz_mul(l, q->x, q->x);
    mpz_mul_ui(l, l, 3);
    mpz_add(l, l, c->a);
    mpz_mul_2exp(t, q->y, 1);
    if (!mpz_invert(t, t, c->p)) {
        ok = false;
        goto out;
    }
    mpz_mul(l, l, t);
    mpz_mod(l, l, c->p);
    /* x3 = l^2 - 2x, y3 = l(x - x3) - y */
    mpz_mul(x3, l, l);
    mpz_submul_ui(x3, q->x, 2);
    mpz_mod(x3, x3, c->p);
    mpz_sub(t, q->x, x3);
    mpz_mul(t, t, l);
    mpz_sub(t, t, q->y);
    mpz_mod(r->y, t, c->p);
    mpz_set(r->x, x3);
    r->inf = false;
out:
    mpz_clears(l, t, x3, NULL);
    return ok;
}

/* r = q + s; false if a needed inverse does not exist */
static bool point_add(const EccCurve *c, EccPoint *r, const EccPoint *q,
                      const EccPoint *s)
{
    mpz_t l, t, x3;
    bool ok = true;

    if (q->inf) {
        point_set(r, s);
        return true;
    }
    if (s->inf) {
        point_set(r, q);
        return true;
    }
    if (mpz_cmp(q->x, s->x) == 0) {
        if (mpz_cmp(q->y, s->y) == 0) {
            return point_double(c, r, q);
        }
        r->inf = true;
        return true;
    }
    mpz_inits(l, t, x3, NULL);
    /* l = (y2 - y1) / (x2 - x1) */
    mpz_sub(l, s->y, q->y);
    mpz_sub(t, s->x, q->x);
    mpz_mod(t, t, c->p);
    if (!mpz_invert(t, t, c->p)) {
        ok = false;
        goto out;
    }
    mpz_mul(l, l, t);
    mpz_mod(l, l, c->p);
    mpz_mul(x3, l, l);
    mpz_sub(x3, x3, q->x);
    mpz_sub(x3, x3, s->x);
    mpz_mod(x3, x3, c->p);
    mpz_sub(t, q->x, x3);
    mpz_mul(t, t, l);
    mpz_sub(t, t, q->y);
    mpz_mod(r->y, t, c->p);
    mpz_set(r->x, x3);
    r->inf = false;
out:
    mpz_clears(l, t, x3, NULL);
    return ok;
}

/* Left-to-right double and add; false if arithmetic failed (p not prime) */
static bool point_mul(const EccCurve *c, EccPoint *r, const mpz_t k,
                      const EccPoint *q)
{
    EccPoint acc, tmp;
    bool ok = true;
    long i;

    point_init(&acc);
    point_init(&tmp);
    for (i = mpz_sizeinbase(k, 2); ok && i-- > 0;) {
        ok = point_double(c, &tmp, &acc);
        point_set(&acc, &tmp);
        if (ok && mpz_tstbit(k, i)) {
            ok = point_add(c, &tmp, &acc, q);
            point_set(&acc, &tmp);
        }
    }
    if (ok) {
        point_set(r, &acc);
    }
    point_clear(&acc);
    point_clear(&tmp);
    return ok;
}

bool ccp_ecc_execute(unsigned op, const uint8_t *src, uint8_t *dst)
{
    mpz_t m, a, b, r;
    bool ok = true;

    switch (op) {
    case CCP_ECC_OP_MMUL:
    case CCP_ECC_OP_MADD:
    case CCP_ECC_OP_MINV:
    case CCP_ECC_OP_PMUL:
        break;
    default:
        return false;
    }

    memset(dst, 0, CCP_ECC_DST_SIZE);
    mpz_inits(m, a, b, r, NULL);
    slot_get(m, src, 0);
    if (mpz_sgn(m) == 0) {
        goto out;
    }

    if (op == CCP_ECC_OP_PMUL) {
        EccCurve c;
        EccPoint q, res;

        mpz_init(c.p);
        mpz_init(c.a);
        mpz_set(c.p, m);
        slot_get(c.a, src, 4);
        mpz_mod(c.a, c.a, c.p);
        point_init(&q);
        point_init(&res);
        slot_get(q.x, src, 1);
        slot_get(q.y, src, 2);
        slot_get(b, src, 5);
        mpz_mod(q.x, q.x, c.p);
        mpz_mod(q.y, q.y, c.p);
        /* The affine encoding of the point at infinity is (0, 0) */
        q.inf = mpz_sgn(q.x) == 0 && mpz_sgn(q.y) == 0;
        if (point_mul(&c, &res, b, &q) && !res.inf) {
            slot_put(dst, res.x);
            slot_put(dst + CCP_ECC_Y_OFFSET, res.y);
        }
        point_clear(&q);
        point_clear(&res);
        mpz_clear(c.p);
        mpz_clear(c.a);
        goto out;
    }

    slot_get(a, src, 1);
    if (op == CCP_ECC_OP_MINV) {
        ok = mpz_invert(r, a, m) != 0;
    } else {
        slot_get(b, src, 2);
        if (op == CCP_ECC_OP_MMUL) {
            mpz_mul(r, a, b);
        } else {
            mpz_add(r, a, b);
        }
        mpz_mod(r, r, m);
    }
    if (ok) {
        slot_put(dst, r);
    }
out:
    mpz_clears(m, a, b, r, NULL);
    return true;
}
