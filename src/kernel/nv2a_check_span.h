/* CHECK-ONLY geometry coverage helpers. Coordinates are logical pixels;
 * w is guest clip-w, not metres. No renderer policy is derived here. */
#ifndef NV2A_CHECK_SPAN_H
#define NV2A_CHECK_SPAN_H
#include <stdint.h>
#include <math.h>
typedef struct {
    uint64_t refs, invalid, nonforward, positive, over2000, outside;
    uint64_t tri_inside, tri_outside, tri_crossing, tri_invalid;
    float wmin, wmax;
} Nv2aCheckSpan;
/* Codes 0..15 are XY outcodes; 16 means not finite or not forward. */
static inline unsigned nv2a_check_outcode(const float p[4], float width, float height)
{
    if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2])
        || !isfinite(p[3]) || !(p[3] > 0))
        return 16;
    return (p[0] < 0 ? 1u : 0u) | (p[0] > width ? 2u : 0u)
         | (p[1] < 0 ? 4u : 0u) | (p[1] > height ? 8u : 0u);
}
static inline void nv2a_check_span_vertex(Nv2aCheckSpan *s, const float p[4],
                                          float width, float height)
{
    s->refs++;
    if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2]) || !isfinite(p[3])) {
        s->invalid++;
        return;
    }
    if (!(p[3] > 0)) { s->nonforward++; return; }
    if (!s->positive || p[3] < s->wmin) s->wmin = p[3];
    if (!s->positive || p[3] > s->wmax) s->wmax = p[3];
    s->positive++;
    if (p[3] > 2000.0f) s->over2000++;
    if (nv2a_check_outcode(p, width, height)) s->outside++;
}
static inline void nv2a_check_span_triangle(Nv2aCheckSpan *s, unsigned a,
                                            unsigned b, unsigned c)
{
    if ((a | b | c) & 16) s->tri_invalid++;
    else if (!(a | b | c)) s->tri_inside++;
    else if (a & b & c) s->tri_outside++;
    else s->tri_crossing++; /* conservative: may still miss the rectangle */
}
#endif
