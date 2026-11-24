/* Copyright (C) 2026 Benoit Colson */
/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Dual-Path Floating-Point & Math Engine for hobbyOS BASIC */

#ifndef HOBBYOS_BASIC_MATH_H
#define HOBBYOS_BASIC_MATH_H

#include "hobbyos.h"

#ifndef FP_HERE
#define FP_HERE(ptr) ((void __far *)(((unsigned long)get_ds() << 16) | (uint16_t)(ptr)))
#endif

typedef uint32_t basic_float_t;

/* Common Constants in IEEE-754 32-bit Single Precision */
#define FLOAT_ZERO          0x00000000UL
#define FLOAT_ONE           0x3F800000UL  /* 1.0 */
#define FLOAT_TWO           0x40000000UL  /* 2.0 */
#define FLOAT_HALF          0x3F000000UL  /* 0.5 */
#define FLOAT_PI            0x40490FDBUL  /* 3.14159265 */
#define FLOAT_PI_DIV2       0x3FC90FDBUL  /* 1.57079632 */
#define FLOAT_PI_DIV4       0x3F490FDBUL  /* 0.78539816 */
#define FLOAT_LN2           0x3F317218UL  /* 0.69314718 */
#define FLOAT_DEG_TO_RAD    0x3C8EFA35UL  /* PI / 180 = 0.0174532925 */

/* Runtime FPU presence flag */
static uint8_t s_basic_has_fpu = 0;

/* --------------------------------------------------------------------------
 * Hardware 8087 Inline Assembly Primitives
 * -------------------------------------------------------------------------- */

#ifdef __WATCOMC__
void fpu_hw_init(void);
#pragma aux fpu_hw_init = \
    "fninit";

void fpu_hw_add(const basic_float_t *a, const basic_float_t *b, basic_float_t *res);
#pragma aux fpu_hw_add = \
    "fld dword ptr ss:[bx]" \
    "fadd dword ptr ss:[si]" \
    "fstp dword ptr ss:[di]" \
    parm [bx] [si] [di];

void fpu_hw_sub(const basic_float_t *a, const basic_float_t *b, basic_float_t *res);
#pragma aux fpu_hw_sub = \
    "fld dword ptr ss:[bx]" \
    "fsub dword ptr ss:[si]" \
    "fstp dword ptr ss:[di]" \
    parm [bx] [si] [di];

void fpu_hw_mul(const basic_float_t *a, const basic_float_t *b, basic_float_t *res);
#pragma aux fpu_hw_mul = \
    "fld dword ptr ss:[bx]" \
    "fmul dword ptr ss:[si]" \
    "fstp dword ptr ss:[di]" \
    parm [bx] [si] [di];

void fpu_hw_div(const basic_float_t *a, const basic_float_t *b, basic_float_t *res);
#pragma aux fpu_hw_div = \
    "fld dword ptr ss:[bx]" \
    "fdiv dword ptr ss:[si]" \
    "fstp dword ptr ss:[di]" \
    parm [bx] [si] [di];

void fpu_hw_sqr(const basic_float_t *a, basic_float_t *res);
#pragma aux fpu_hw_sqr = \
    "fld dword ptr ss:[bx]" \
    "fsqrt" \
    "fstp dword ptr ss:[di]" \
    parm [bx] [di];

#else
static void fpu_hw_init(void) {}
static void fpu_hw_add(const basic_float_t *a, const basic_float_t *b, basic_float_t *res) { (void)a; (void)b; (void)res; }
static void fpu_hw_sub(const basic_float_t *a, const basic_float_t *b, basic_float_t *res) { (void)a; (void)b; (void)res; }
static void fpu_hw_mul(const basic_float_t *a, const basic_float_t *b, basic_float_t *res) { (void)a; (void)b; (void)res; }
static void fpu_hw_div(const basic_float_t *a, const basic_float_t *b, basic_float_t *res) { (void)a; (void)b; (void)res; }
static void fpu_hw_sqr(const basic_float_t *a, basic_float_t *res) { (void)a; (void)res; }
#endif

/* --------------------------------------------------------------------------
 * Freestanding 8086 16-bit Math Primitives (Zero Library Dependencies)
 * -------------------------------------------------------------------------- */

#ifdef __WATCOMC__
static uint32_t mul16(uint16_t a, uint16_t b);
#pragma aux mul16 = \
    "mul dx" \
    parm [ax] [dx] value [dx ax];
#else
static uint32_t mul16(uint16_t a, uint16_t b) {
    return (uint32_t)a * (uint32_t)b;
}
#endif

static uint16_t u32_div10(uint32_t *val) {
    uint16_t hi = (uint16_t)(*val >> 16);
    uint16_t lo = (uint16_t)(*val & 0xFFFF);
    uint16_t q_hi = hi / 10;
    uint16_t r_hi = hi % 10;
    uint16_t d_hi = (r_hi << 4) + (lo >> 12);
    uint16_t q_mid = d_hi / 10;
    uint16_t r_mid = d_hi % 10;
    uint16_t d_lo = (r_mid << 12) + (lo & 0x0FFF);
    uint16_t q_lo = d_lo / 10;
    uint16_t rem  = d_lo % 10;
    *val = ((uint32_t)q_hi << 16) | ((uint32_t)(q_mid << 12) + q_lo);
    return rem;
}

static basic_float_t soft_from_int(int16_t v) {
    uint32_t sign = 0;
    uint16_t u;
    int msb = 0;
    uint16_t tmp;
    uint32_t exp;
    uint32_t mant;

    if (v == 0) return FLOAT_ZERO;
    if (v < 0) {
        sign = 0x80000000UL;
        u = (uint16_t)(-v);
    } else {
        u = (uint16_t)v;
    }

    tmp = u;
    if (tmp >= 256) { msb += 8; tmp >>= 8; }
    if (tmp >= 16)  { msb += 4; tmp >>= 4; }
    if (tmp >= 4)   { msb += 2; tmp >>= 2; }
    if (tmp >= 2)   { msb += 1; tmp >>= 1; }

    exp = (uint32_t)(127 + msb);
    if (23 >= msb) {
        mant = ((uint32_t)u << (23 - msb)) & 0x007FFFFFUL;
    } else {
        mant = ((uint32_t)u >> (msb - 23)) & 0x007FFFFFUL;
    }

    return sign | (exp << 23) | mant;
}

static basic_float_t soft_from_uint32(uint32_t u) {
    int msb = 0;
    uint32_t tmp;
    uint32_t exp;
    uint32_t mant;

    if (u == 0) return FLOAT_ZERO;

    tmp = u;
    if (tmp >= 0x10000UL) { msb += 16; tmp >>= 16; }
    if (tmp >= 0x100UL)   { msb += 8;  tmp >>= 8; }
    if (tmp >= 0x10UL)    { msb += 4;  tmp >>= 4; }
    if (tmp >= 0x4UL)     { msb += 2;  tmp >>= 2; }
    if (tmp >= 0x2UL)     { msb += 1;  tmp >>= 1; }

    exp = (uint32_t)(127 + msb);
    if (23 >= msb) {
        mant = (u << (23 - msb)) & 0x007FFFFFUL;
    } else {
        mant = (u >> (msb - 23)) & 0x007FFFFFUL;
    }

    return (exp << 23) | mant;
}

static basic_float_t soft_from_uint(uint16_t u) {
    return soft_from_uint32((uint32_t)u);
}

static int16_t soft_to_int(basic_float_t f) {
    uint32_t sign = f & 0x80000000UL;
    int exp = (int)((f >> 23) & 0xFF) - 127;
    uint32_t mant;
    int32_t val;

    if (exp < 0) return 0;
    if (exp > 15) {
        /* Overflow clamp */
        return sign ? -32768 : 32767;
    }

    mant = (f & 0x007FFFFFUL) | 0x00800000UL;
    if (23 >= exp) {
        val = (int32_t)(mant >> (23 - exp));
    } else {
        val = (int32_t)(mant << (exp - 23));
    }

    return sign ? (int16_t)(-val) : (int16_t)val;
}

static uint32_t soft_to_uint32(basic_float_t f) {
    int exp = (int)((f >> 23) & 0xFF) - 127;
    uint32_t mant;

    if (exp < 0) return 0;
    if (exp > 31) return 0xFFFFFFFFUL;

    mant = (f & 0x007FFFFFUL) | 0x00800000UL;
    if (exp >= 23) {
        return mant << (exp - 23);
    } else {
        return mant >> (23 - exp);
    }
}

static basic_float_t soft_add_sub(basic_float_t a, basic_float_t b, int is_sub) {
    uint32_t sa = a & 0x80000000UL;
    uint32_t sb = (b & 0x80000000UL) ^ (is_sub ? 0x80000000UL : 0);
    int ea = (int)((a >> 23) & 0xFF);
    int eb = (int)((b >> 23) & 0xFF);
    uint32_t ma, mb, mr;
    int diff, er;
    uint32_t sr;
    uint32_t tmp_m, tmp_s;
    int tmp_e;

    if (ea == 0) return b ^ (is_sub ? 0x80000000UL : 0);
    if (eb == 0) return a;

    ma = (a & 0x007FFFFFUL) | 0x00800000UL;
    mb = (b & 0x007FFFFFUL) | 0x00800000UL;

    if (ea < eb) {
        tmp_m = ma; ma = mb; mb = tmp_m;
        tmp_e = ea; ea = eb; eb = tmp_e;
        tmp_s = sa; sa = sb; sb = tmp_s;
    }

    diff = ea - eb;
    if (diff > 25) {
        mb = 0;
    } else {
        mb >>= diff;
    }

    er = ea;
    if (sa == sb) {
        sr = sa;
        mr = ma + mb;
        if (mr & 0x01000000UL) {
            mr >>= 1;
            er++;
        }
    } else {
        if (ma >= mb) {
            sr = sa;
            mr = ma - mb;
        } else {
            sr = sb;
            mr = mb - ma;
        }
        if (mr == 0) return FLOAT_ZERO;
        while ((mr & 0x00800000UL) == 0 && er > 0) {
            mr <<= 1;
            er--;
        }
    }

    if (er <= 0) return FLOAT_ZERO;
    if (er >= 255) return sr | 0x7F800000UL; /* Infinity */

    return sr | ((uint32_t)er << 23) | (mr & 0x007FFFFFUL);
}

static basic_float_t soft_mul(basic_float_t a, basic_float_t b) {
    uint32_t sa = a & 0x80000000UL;
    uint32_t sb = b & 0x80000000UL;
    uint32_t sr = sa ^ sb;
    int ea = (int)((a >> 23) & 0xFF);
    int eb = (int)((b >> 23) & 0xFF);
    uint32_t ma, mb;
    uint32_t a_hi, a_lo, b_hi, b_lo;
    uint32_t p_hh, p_hl, p_lh, prod;
    int er;

    if (ea == 0 || eb == 0) return FLOAT_ZERO;

    er = ea + eb - 127;
    ma = (a & 0x007FFFFFUL) | 0x00800000UL;
    mb = (b & 0x007FFFFFUL) | 0x00800000UL;

    /* 24x24 bit mantissa multiply into top 32 bits:
     * Split each into 12-bit halves: ma = (a_hi << 12) + a_lo */
    {
        uint16_t a_hi = (uint16_t)(ma >> 12);
        uint16_t a_lo = (uint16_t)(ma & 0xFFFUL);
        uint16_t b_hi = (uint16_t)(mb >> 12);
        uint16_t b_lo = (uint16_t)(mb & 0xFFFUL);

        p_hh = mul16(a_hi, b_hi);
        p_hl = mul16(a_hi, b_lo);
        p_lh = mul16(a_lo, b_hi);
    }

    /* prod = (p_hh << 24) + ((p_hl + p_lh) << 12) + (p_ll)
     * Normalized result mantissa fits in bit 23: */
    prod = (p_hh << 1) + ((p_hl + p_lh) >> 11);

    if (prod & 0x01000000UL) {
        prod >>= 1;
        er++;
    }

    if (er <= 0) return FLOAT_ZERO;
    if (er >= 255) return sr | 0x7F800000UL;

    return sr | ((uint32_t)er << 23) | (prod & 0x007FFFFFUL);
}

static basic_float_t soft_div(basic_float_t a, basic_float_t b) {
    uint32_t sa = a & 0x80000000UL;
    uint32_t sb = b & 0x80000000UL;
    uint32_t sr = sa ^ sb;
    int ea = (int)((a >> 23) & 0xFF);
    int eb = (int)((b >> 23) & 0xFF);
    uint32_t ma, mb, q;
    int er, i;

    if (eb == 0) return sr | 0x7F800000UL; /* Divide by zero -> inf */
    if (ea == 0) return FLOAT_ZERO;

    er = ea - eb + 127;
    ma = (a & 0x007FFFFFUL) | 0x00800000UL;
    mb = (b & 0x007FFFFFUL) | 0x00800000UL;

    if (ma < mb) {
        ma <<= 1;
        er--;
    }

    /* 24-step restoring division */
    q = 0;
    for (i = 0; i < 24; i++) {
        q <<= 1;
        if (ma >= mb) {
            ma -= mb;
            q |= 1;
        }
        ma <<= 1;
    }

    if (er <= 0) return FLOAT_ZERO;
    if (er >= 255) return sr | 0x7F800000UL;

    return sr | ((uint32_t)er << 23) | (q & 0x007FFFFFUL);
}

static basic_float_t soft_sqr(basic_float_t a) {
    basic_float_t y;
    int i;

    if (a == 0 || a == 0x80000000UL) return FLOAT_ZERO;
    if (a & 0x80000000UL) return FLOAT_ZERO; /* Negative input -> domain error */

    /* Initial guess via exponent halving */
    y = (a >> 1) + 0x1FC00000UL;

    for (i = 0; i < 4; i++) {
        basic_float_t div = soft_div(a, y);
        y = soft_mul(FLOAT_HALF, soft_add_sub(y, div, 0));
    }
    return y;
}

/* --------------------------------------------------------------------------
 * Dispatch API (Switches between Hardware 8087 and Soft-Float 32)
 * -------------------------------------------------------------------------- */

static void basic_math_init(void) {
    s_basic_has_fpu = (uint8_t)os_fpu_present();
    if (s_basic_has_fpu) {
        fpu_hw_init();
    }
}

static basic_float_t basic_float_add(basic_float_t a, basic_float_t b) {
    if (s_basic_has_fpu) {
        basic_float_t res;
        fpu_hw_add(&a, &b, &res);
        return res;
    }
    return soft_add_sub(a, b, 0);
}

static basic_float_t basic_float_sub(basic_float_t a, basic_float_t b) {
    if (s_basic_has_fpu) {
        basic_float_t res;
        fpu_hw_sub(&a, &b, &res);
        return res;
    }
    return soft_add_sub(a, b, 1);
}

static basic_float_t basic_float_mul(basic_float_t a, basic_float_t b) {
    if (s_basic_has_fpu) {
        basic_float_t res;
        fpu_hw_mul(&a, &b, &res);
        return res;
    }
    return soft_mul(a, b);
}

static basic_float_t basic_float_div(basic_float_t a, basic_float_t b) {
    if (s_basic_has_fpu) {
        basic_float_t res;
        fpu_hw_div(&a, &b, &res);
        return res;
    }
    return soft_div(a, b);
}

static basic_float_t basic_float_sqr(basic_float_t a) {
    if (s_basic_has_fpu) {
        basic_float_t res;
        fpu_hw_sqr(&a, &res);
        return res;
    }
    return soft_sqr(a);
}

static int basic_float_cmp(basic_float_t a, basic_float_t b) {
    uint32_t sa = a & 0x80000000UL;
    uint32_t sb = b & 0x80000000UL;

    if (a == b) return 0;
    if ((a & 0x7FFFFFFFUL) == 0 && (b & 0x7FFFFFFFUL) == 0) return 0; /* +0 == -0 */

    if (sa != sb) {
        return sa ? -1 : 1;
    }
    if (sa) {
        /* Both negative */
        return (a > b) ? -1 : 1;
    }
    return (a < b) ? -1 : 1;
}

/* --------------------------------------------------------------------------
 * Transcendental Functions (SIN, COS, TAN, ATN, EXP, LOG, POW)
 * -------------------------------------------------------------------------- */

/* Core polynomial for sin(y) for y in [0, PI/4]:
 * sin(y) approx y * (1 - y^2/6 + y^4/120 - y^6/5040) */
static basic_float_t sin_poly_core(basic_float_t y) {
    basic_float_t c1 = 0x3E2AAAABUL; /* 1/6 = 0.16666667 */
    basic_float_t c2 = 0x3C088889UL; /* 1/120 = 0.00833333 */
    basic_float_t c3 = 0x39500D01UL; /* 1/5040 = 0.00019841 */
    basic_float_t y2 = basic_float_mul(y, y);
    basic_float_t y4 = basic_float_mul(y2, y2);
    basic_float_t t = basic_float_sub(FLOAT_ONE, basic_float_mul(c1, y2));
    t = basic_float_add(t, basic_float_mul(c2, y4));
    t = basic_float_sub(t, basic_float_mul(c3, basic_float_mul(y4, y2)));
    return basic_float_mul(y, t);
}

/* Core polynomial for cos(y) for y in [0, PI/4]:
 * cos(y) approx 1 - y^2/2 + y^4/24 - y^6/720 */
static basic_float_t cos_poly_core(basic_float_t y) {
    basic_float_t c1 = 0x3F000000UL; /* 1/2 = 0.5 */
    basic_float_t c2 = 0x3D2AAAABUL; /* 1/24 = 0.04166667 */
    basic_float_t c3 = 0x3AB60B61UL; /* 1/720 = 0.00138889 */
    basic_float_t y2 = basic_float_mul(y, y);
    basic_float_t y4 = basic_float_mul(y2, y2);
    basic_float_t t = basic_float_sub(FLOAT_ONE, basic_float_mul(c1, y2));
    t = basic_float_add(t, basic_float_mul(c2, y4));
    t = basic_float_sub(t, basic_float_mul(c3, basic_float_mul(y4, y2)));
    return t;
}

static basic_float_t basic_float_sin(basic_float_t x) {
    basic_float_t res;
    int is_neg = 0;

    if (x & 0x80000000UL) {
        is_neg = 1;
        x ^= 0x80000000UL;
    }

    /* Range reduction mod 2*PI */
    while (basic_float_cmp(x, 0x40C90FDBUL /* 6.2831853 */) >= 0) {
        x = basic_float_sub(x, 0x40C90FDBUL);
    }
    if (basic_float_cmp(x, FLOAT_PI) > 0) {
        x = basic_float_sub(x, FLOAT_PI);
        is_neg = !is_neg;
    }
    if (basic_float_cmp(x, FLOAT_PI_DIV2) > 0) {
        x = basic_float_sub(FLOAT_PI, x);
    }

    if (basic_float_cmp(x, FLOAT_PI_DIV4) > 0) {
        res = cos_poly_core(basic_float_sub(FLOAT_PI_DIV2, x));
    } else {
        res = sin_poly_core(x);
    }

    return is_neg ? (res ^ 0x80000000UL) : res;
}

static basic_float_t basic_float_cos(basic_float_t x) {
    basic_float_t res;
    int is_neg = 0;

    x &= 0x7FFFFFFFUL; /* cos is even */

    /* Range reduction mod 2*PI */
    while (basic_float_cmp(x, 0x40C90FDBUL) >= 0) {
        x = basic_float_sub(x, 0x40C90FDBUL);
    }
    if (basic_float_cmp(x, FLOAT_PI) > 0) {
        x = basic_float_sub(0x40C90FDBUL, x);
    }
    if (basic_float_cmp(x, FLOAT_PI_DIV2) > 0) {
        x = basic_float_sub(FLOAT_PI, x);
        is_neg = 1;
    }

    if (basic_float_cmp(x, FLOAT_PI_DIV4) > 0) {
        res = sin_poly_core(basic_float_sub(FLOAT_PI_DIV2, x));
    } else {
        res = cos_poly_core(x);
    }

    return is_neg ? (res ^ 0x80000000UL) : res;
}

static basic_float_t basic_float_tan(basic_float_t x) {
    return basic_float_div(basic_float_sin(x), basic_float_cos(x));
}

static basic_float_t basic_float_atn(basic_float_t x) {
    int is_neg = 0;
    int is_inv = 0;
    basic_float_t res;
    basic_float_t c1 = 0x3EAAAAABUL; /* 1/3 = 0.33333333 */
    basic_float_t c2 = 0x3E4CCCCDUL; /* 1/5 = 0.2 */
    basic_float_t f_sqrt2_m1 = 0x3ED413CDUL; /* 0.41421356 */

    if (x & 0x80000000UL) {
        is_neg = 1;
        x ^= 0x80000000UL;
    }

    if (basic_float_cmp(x, FLOAT_ONE) > 0) {
        is_inv = 1;
        x = basic_float_div(FLOAT_ONE, x);
    }

    if (basic_float_cmp(x, f_sqrt2_m1) > 0) {
        /* atan(x) = pi/4 + atan((x - 1) / (x + 1)) */
        basic_float_t x_red = basic_float_div(basic_float_sub(x, FLOAT_ONE), basic_float_add(x, FLOAT_ONE));
        basic_float_t xr2 = basic_float_mul(x_red, x_red);
        basic_float_t t = basic_float_sub(FLOAT_ONE, basic_float_mul(c1, xr2));
        t = basic_float_add(t, basic_float_mul(c2, basic_float_mul(xr2, xr2)));
        res = basic_float_add(FLOAT_PI_DIV4, basic_float_mul(x_red, t));
    } else {
        basic_float_t x2 = basic_float_mul(x, x);
        basic_float_t t = basic_float_sub(FLOAT_ONE, basic_float_mul(c1, x2));
        t = basic_float_add(t, basic_float_mul(c2, basic_float_mul(x2, x2)));
        res = basic_float_mul(x, t);
    }

    if (is_inv) {
        res = basic_float_sub(FLOAT_PI_DIV2, res);
    }

    return is_neg ? (res ^ 0x80000000UL) : res;
}

static basic_float_t basic_float_exp(basic_float_t x) {
    /* Taylor series: exp(x) = 1 + x + x^2/2 + x^3/6 + x^4/24 + x^5/120 */
    basic_float_t term = FLOAT_ONE;
    basic_float_t sum = FLOAT_ONE;
    int i;
    for (i = 1; i <= 8; i++) {
        term = basic_float_div(basic_float_mul(term, x), soft_from_int((int16_t)i));
        sum = basic_float_add(sum, term);
    }
    return sum;
}

static basic_float_t basic_float_log(basic_float_t x) {
    /* Natural log ln(x) for x > 0 using series ln((1+y)/(1-y)) = 2*(y + y^3/3 + y^5/5) */
    basic_float_t y, y2, term, sum;
    int exp_adj = 0;
    int i;

    if (x == 0 || (x & 0x80000000UL)) return FLOAT_ZERO; /* Domain error */

    /* Bring x close to 1.0 */
    while (basic_float_cmp(x, FLOAT_TWO) >= 0) {
        x = basic_float_mul(x, FLOAT_HALF);
        exp_adj++;
    }
    while (basic_float_cmp(x, FLOAT_HALF) < 0) {
        x = basic_float_mul(x, FLOAT_TWO);
        exp_adj--;
    }

    /* y = (x - 1) / (x + 1) */
    y = basic_float_div(basic_float_sub(x, FLOAT_ONE), basic_float_add(x, FLOAT_ONE));
    y2 = basic_float_mul(y, y);
    term = y;
    sum = y;

    for (i = 3; i <= 9; i += 2) {
        term = basic_float_mul(term, y2);
        sum = basic_float_add(sum, basic_float_div(term, soft_from_int((int16_t)i)));
    }
    sum = basic_float_mul(FLOAT_TWO, sum);

    if (exp_adj != 0) {
        sum = basic_float_add(sum, basic_float_mul(soft_from_int((int16_t)exp_adj), FLOAT_LN2));
    }
    return sum;
}

static basic_float_t basic_float_pow(basic_float_t base, basic_float_t exp) {
    /* Integer exponent fast path */
    int16_t iexp = soft_to_int(exp);
    basic_float_t f_iexp = soft_from_int(iexp);
    if (f_iexp == exp && iexp >= 0 && iexp <= 30) {
        basic_float_t r = FLOAT_ONE;
        basic_float_t b = base;
        int p = iexp;
        while (p > 0) {
            if (p & 1) r = basic_float_mul(r, b);
            b = basic_float_mul(b, b);
            p >>= 1;
        }
        return r;
    }
    /* a^b = exp(b * log(a)) */
    return basic_float_exp(basic_float_mul(exp, basic_float_log(base)));
}

/* --------------------------------------------------------------------------
 * String Conversion & Formatting
 * -------------------------------------------------------------------------- */

static basic_float_t basic_parse_float_far(const uint8_t __far *s, uint16_t *pos) {
    uint16_t p = *pos;
    uint32_t int_part = 0;
    uint32_t frac_part = 0;
    uint32_t frac_div = 1;
    int is_neg = 0;
    int has_dot = 0;
    basic_float_t res;

    while (s[p] == ' ' || s[p] == '\t') p++;
    if (s[p] == '-') {
        is_neg = 1;
        p++;
    } else if (s[p] == '+') {
        p++;
    }

    while (s[p] >= '0' && s[p] <= '9') {
        int_part = (int_part << 3) + (int_part << 1) + (uint32_t)(s[p] - '0');
        p++;
    }

    if (s[p] == '.') {
        has_dot = 1;
        p++;
        while (s[p] >= '0' && s[p] <= '9') {
            if (frac_div < 10000000UL) {
                frac_part = (frac_part << 3) + (frac_part << 1) + (uint32_t)(s[p] - '0');
                frac_div = (frac_div << 3) + (frac_div << 1);
            }
            p++;
        }
    }

    (void)has_dot;
    res = soft_from_uint32(int_part);
    if (frac_part > 0) {
        basic_float_t f_frac = soft_from_uint32(frac_part);
        basic_float_t f_div  = soft_from_uint32(frac_div);
        res = basic_float_add(res, basic_float_div(f_frac, f_div));
    }

    /* Exponent E/e/D/d */
    if (s[p] == 'E' || s[p] == 'e' || s[p] == 'D' || s[p] == 'd') {
        int exp_neg = 0;
        int exp_val = 0;
        int k;
        p++;
        if (s[p] == '-') { exp_neg = 1; p++; }
        else if (s[p] == '+') { p++; }
        while (s[p] >= '0' && s[p] <= '9') {
            exp_val = (exp_val * 10) + (s[p] - '0');
            p++;
        }
        for (k = 0; k < exp_val; k++) {
            basic_float_t f_ten = soft_from_int(10);
            if (exp_neg) res = basic_float_div(res, f_ten);
            else         res = basic_float_mul(res, f_ten);
        }
    }

    *pos = p;
    return is_neg ? (res ^ 0x80000000UL) : res;
}

static basic_float_t basic_parse_float_str(const char *s, uint16_t *pos) {
    return basic_parse_float_far((const uint8_t __far *)FP_HERE(s), pos);
}

static basic_float_t basic_atof(const char *s) {
    uint16_t pos = 0;
    return basic_parse_float_str(s, &pos);
}

static uint16_t basic_format_float(basic_float_t f, char *buf, uint16_t max_len) {
    uint16_t bp = 0;
    basic_float_t val = f;
    int is_neg = 0;
    int exp10 = 0;
    uint32_t scaled;
    char digits[10];
    int dp = 0;
    int k;

    if (max_len < 16) return 0;

    if (val & 0x80000000UL) {
        is_neg = 1;
        val ^= 0x80000000UL;
    }

    if (val == 0) {
        buf[bp++] = ' ';
        buf[bp++] = '0';
        buf[bp] = '\0';
        return bp;
    }

    /* Print sign: space for positive, '-' for negative (standard GW-BASIC) */
    buf[bp++] = is_neg ? '-' : ' ';

    /* Normalize into 1.0 <= val < 10.0 range to get decimal exponent */
    while (basic_float_cmp(val, soft_from_int(10)) >= 0 && exp10 < 38) {
        val = basic_float_div(val, soft_from_int(10));
        exp10++;
    }
    while (basic_float_cmp(val, FLOAT_ONE) < 0 && exp10 > -38) {
        val = basic_float_mul(val, soft_from_int(10));
        exp10--;
    }

    /* Extract 6 significant digits */
    /* Multiply by 100,000 to get a 6-digit integer: */
    {
        basic_float_t f_100k = soft_from_uint(50000);
        f_100k = basic_float_mul(f_100k, FLOAT_TWO); /* 100,000 */
        val = basic_float_mul(val, f_100k);
    }

    /* Round half to even */
    val = basic_float_add(val, FLOAT_HALF);
    scaled = soft_to_uint32(val);
    if (scaled >= 1000000UL) {
        u32_div10(&scaled);
        exp10++;
    }

    /* Produce digits in reverse */
    for (k = 0; k < 6; k++) {
        digits[dp++] = (char)('0' + u32_div10(&scaled));
    }

    /* Standard fixed vs exponential display */
    if (exp10 >= -1 && exp10 <= 6) {
        /* Fixed format e.g. 123.456 or 0.123456 */
        int int_digits = exp10 + 1;
        int d_idx = dp - 1;
        if (int_digits <= 0) {
            buf[bp++] = '0';
            buf[bp++] = '.';
            while (int_digits < 0) {
                buf[bp++] = '0';
                int_digits++;
            }
            while (d_idx >= 0 && bp < max_len - 2) {
                buf[bp++] = digits[d_idx--];
            }
        } else {
            while (int_digits > 0 && d_idx >= 0) {
                buf[bp++] = digits[d_idx--];
                int_digits--;
            }
            while (int_digits > 0) {
                buf[bp++] = '0';
                int_digits--;
            }
            if (d_idx >= 0) {
                buf[bp++] = '.';
                while (d_idx >= 0 && bp < max_len - 2) {
                    buf[bp++] = digits[d_idx--];
                }
            }
        }
        /* Strip trailing zeros after '.' */
        while (bp > 1 && buf[bp - 1] == '0') bp--;
        if (bp > 1 && buf[bp - 1] == '.') bp--;
    } else {
        /* Scientific notation: e.g. 1.23456E+08 */
        int d_idx = dp - 1;
        buf[bp++] = digits[d_idx--];
        buf[bp++] = '.';
        while (d_idx >= 0 && bp < max_len - 6) {
            buf[bp++] = digits[d_idx--];
        }
        /* Strip trailing zeros */
        while (bp > 1 && buf[bp - 1] == '0') bp--;
        if (bp > 1 && buf[bp - 1] == '.') bp--;

        buf[bp++] = 'E';
        if (exp10 >= 0) {
            buf[bp++] = '+';
        } else {
            buf[bp++] = '-';
            exp10 = -exp10;
        }
        if (exp10 < 10) buf[bp++] = '0';
        {
            char ebuf[4];
            int ep = 0;
            while (exp10 > 0) {
                ebuf[ep++] = (char)('0' + (exp10 % 10));
                exp10 /= 10;
            }
            while (ep > 0) buf[bp++] = ebuf[--ep];
        }
    }

    buf[bp] = '\0';
    return bp;
}

#endif /* HOBBYOS_BASIC_MATH_H */
