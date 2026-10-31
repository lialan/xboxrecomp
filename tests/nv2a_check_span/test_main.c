#include "../../src/kernel/nv2a_check_span.h"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main(void)
{
    Nv2aCheckSpan s = {0};
    const float p[][4] = {
        {320,240,1,1}, {640,480,1,2000}, {-1,100,1,2001},
        {100,481,1,4000}, {100,100,1,-3}, {100,100,1,0},
        {NAN,100,1,5}, {100,100,1,NAN}, {100,100,1,INFINITY},
        {100,100,NAN,5}, {INFINITY,100,1,5}
    };
    for (unsigned i = 0; i < sizeof p / sizeof p[0]; i++)
        nv2a_check_span_vertex(&s, p[i], 640, 480);
    CHECK(s.refs == 11 && s.invalid == 5 && s.nonforward == 2);
    CHECK(s.positive == 4 && s.wmin == 1 && s.wmax == 4000);
    CHECK(s.over2000 == 2 && s.outside == 2);
    CHECK(nv2a_check_outcode(p[1],640,480) == 0);
    CHECK(nv2a_check_outcode(p[2],640,480) == 1);
    CHECK(nv2a_check_outcode(p[3],640,480) == 8);
    CHECK(nv2a_check_outcode(p[4],640,480) == 16);
    nv2a_check_span_triangle(&s,0,0,0);
    nv2a_check_span_triangle(&s,1,1|4,1|8);
    nv2a_check_span_triangle(&s,1,2,4);
    nv2a_check_span_triangle(&s,16,0,0);
    CHECK(s.tri_inside == 1 && s.tri_outside == 1);
    CHECK(s.tri_crossing == 1 && s.tri_invalid == 1);
    puts("span: finite/negative/zero/NaN/inf, bounds and triangle outcodes passed");
    return 0;
}
