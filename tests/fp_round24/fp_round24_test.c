/* recomp_fp_round24 (bit path) against recomp_fp_round24_ref (frexp/ldexp,
 * the previous implementation): bit-identical results over the edges --
 * zeros, subnormals of double and float, float's range limits, the
 * round-to-even ties and the carries into the exponent, the overflow to
 * infinity, NaNs and infinities -- and over random bit patterns of every
 * exponent. */
#include "recomp_types.h"
#include <stdio.h>
#include <string.h>

RECOMP_TLS struct RecompRegs g_regs;   /* g_fp_cc lives in it */

static uint64_t bits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static double dbl(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }

static unsigned long s_n, s_bad;

static void check(double x)
{
    double a = recomp_fp_round24(x), r = recomp_fp_round24_ref(x);
    s_n++;
    if (bits(a) != bits(r)) {
        if (s_bad++ < 20)
            printf("MISMATCH x=%a (%016llx): bits %a (%016llx) ref %a (%016llx)\n", x,
                   (unsigned long long)bits(x), a, (unsigned long long)bits(a), r,
                   (unsigned long long)bits(r));
    }
}

static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return s_rng;
}

int main(void)
{
    static const double edges[] = {
        0.0, 1.0, 1.5, 0.1, 1e-300, 1e300, 3.4028234663852886e38, 3.4028235677973366e38,
        3.402823669209385e38, 1.1754943508222875e-38, 1.1754942106924411e-38,
        1.401298464324817e-45, 7.006492321624085e-46, 4.9406564584124654e-324,
        2.2250738585072014e-308, 2.2250738585072009e-308, 1.7976931348623157e308,
        16777217.0, 16777219.0, 33554435.0, 0.30000000000000004,
    };
    int i, s, k;
    for (i = 0; i < (int)(sizeof edges / sizeof edges[0]); i++)
        for (s = 0; s < 2; s++)
            check(s ? -edges[i] : edges[i]);
    check(INFINITY); check(-INFINITY); check(NAN); check(-NAN);
    check(dbl(0x7ff0000000000001ull)); check(dbl(0xfff8000000000123ull));
    /* ties and carries: every fraction pattern around the 29-bit cut, at
     * exponents through the whole range */
    for (k = 0; k < 0x7ff; k++)
        for (i = 0; i < 64; i++) {
            uint64_t f = ((uint64_t)(i & 7) << 29) | ((i & 8) ? 0x10000000ull : 0)
                       | ((i & 16) ? 0x0fffffffull : 0) | ((i & 32) ? 0x000ffffe00000000ull : 0);
            check(dbl(((uint64_t)k << 52) | f));
            check(dbl(((uint64_t)k << 52) | (0x000fffffffffffffull & ~(uint64_t)i)));
            check(-dbl(((uint64_t)k << 52) | f));
        }
    for (i = 0; i < 20000000; i++)
        check(dbl(rnd()));
    printf("%lu values, %lu mismatches\n", s_n, s_bad);
    return s_bad != 0;
}
