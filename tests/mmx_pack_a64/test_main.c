/* Exhaust every signed-word input through every PACKUSWB output lane.
 * Runs the NEON implementation on AArch64 and the portable reference path
 * elsewhere; the oracle is independent scalar saturation. */
#define RECOMP_GENERATED_CODE 1
#include "../../templates/runtime/recomp_types.h"
#include <stdio.h>

/* Keep a callable boundary for inspecting AArch64 code generation. */
RecompMmx probe(RecompMmx a, RecompMmx b)
{
    return MMX_PACKUSWB(a, b);
}

int main(void)
{
    for (int x = -32768; x <= 32767; x++) {
        RecompMmx a, b;
        for (int i = 0; i < 4; i++) {
            a.w[i] = (int16_t)(x + i * 8191);
            b.w[i] = (int16_t)(-x + i * 1237);
        }
        RecompMmx r = probe(a, b);
        for (int i = 0; i < 8; i++) {
            int v = i < 4 ? a.w[i] : b.w[i - 4];
            unsigned want = v < 0 ? 0 : v > 255 ? 255 : v;
            if (r.ub[i] != want) {
                fprintf(stderr, "PACKUSWB input=%d lane=%d got=%u want=%u\n",
                        x, i, r.ub[i], want);
                return 1;
            }
        }
    }
    puts("PACKUSWB: all signed16 values, 8 output lanes passed");
    return 0;
}
