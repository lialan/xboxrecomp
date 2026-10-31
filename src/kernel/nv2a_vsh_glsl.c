/**
 * NV2A vertex program -> GLSL, for a back end that runs vertex programs on
 * the GPU (NV2A_RAW_PROGRAM). The output is the body of one GLSL function,
 *
 *     void nv2a_vsh(in vec4 v[16], out vec4 oPos, out vec4 O[13])
 *
 * reading constants from `uniform vec4 u_vc[192]`, that computes exactly what
 * nv2a_vsh_run (nv2a_vsh_interp.c) computes: the same field decoding, the
 * same order of reads and writes within an instruction (all three sources
 * read before either unit writes; the ILU's temp write goes to R1 whenever a
 * MAC op is present), the same out-of-range rules (a temp above 12 reads R0,
 * a relative constant outside 0..191 reads c[0]), the same ILU edge values
 * (rcp of 0 is +INF; rcc's clamps; log of 0 is -INF). O[] holds the output
 * registers by address (3 oD0, 4 oD1, 5 oFog, 9-12 oT0-3), initialised as
 * the interpreter initialises them; the caller applies the interpreter's
 * epilogue (colour clamps, w = +0 -> 1, the anti-aliasing scale).
 *
 * Writes to c[] (the out-to-constant bit) are counted in *writes_c and
 * otherwise ignored, as the interpreter ignores them while CXT_WRITE is off;
 * the executor keeps such programs on the CPU when it is on.
 */
#include "nv2a_vsh_glsl.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static uint32_t field(const uint32_t *ins, int dw, int lo, int width)
{
    return (ins[dw] >> lo) & ((1u << width) - 1u);
}

typedef struct { char *p; size_t cap, len; int over; uint32_t inputs; } Buf;

static void put(Buf *b, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (b->over)
        return;
    va_start(ap, fmt);
    n = vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= b->cap - b->len)
        b->over = 1;
    else
        b->len += (size_t)n;
}

/* A source operand as a GLSL expression: register, swizzle, negation. */
static void src(Buf *b, const uint32_t *ins, int which)
{
    static const char sw[] = "xyzw";
    uint32_t neg, swz, mux, reg;

    if (which == 0) {
        neg = field(ins, 1, 8, 1); swz = field(ins, 1, 0, 8);
        mux = field(ins, 2, 26, 2); reg = field(ins, 2, 28, 4);
    } else if (which == 1) {
        neg = field(ins, 2, 25, 1); swz = field(ins, 2, 17, 8);
        mux = field(ins, 2, 11, 2); reg = field(ins, 2, 13, 4);
    } else {
        neg = field(ins, 2, 10, 1); swz = field(ins, 2, 2, 8);
        mux = field(ins, 3, 28, 2);
        reg = (field(ins, 2, 0, 2) << 2) | field(ins, 3, 30, 2);
    }
    put(b, "%s", neg ? "-" : "");
    if (mux == 1) {
        if (reg == 12)
            put(b, "oPos");
        else
            put(b, "R[%u]", reg < 12 ? reg : 0u);
    } else if (mux == 2) {
        put(b, "v[%u]", field(ins, 1, 9, 4));
        b->inputs |= 1u << field(ins, 1, 9, 4);
    } else {
        uint32_t ci = field(ins, 1, 13, 8);
        if (field(ins, 3, 1, 1))
            put(b, "vc_rel(%u + a0)", ci);
        else
            put(b, "u_vc[%u]", ci < 192 ? ci : 0u);
    }
    put(b, ".%c%c%c%c", sw[(swz >> 6) & 3], sw[(swz >> 4) & 3], sw[(swz >> 2) & 3],
        sw[swz & 3]);
}

static void write_masked(Buf *b, const char *dst, const char *val, uint32_t mask)
{
    if (mask & 8) put(b, " %s.x = %s.x;", dst, val);
    if (mask & 4) put(b, " %s.y = %s.y;", dst, val);
    if (mask & 2) put(b, " %s.z = %s.z;", dst, val);
    if (mask & 1) put(b, " %s.w = %s.w;", dst, val);
}

/* The helpers the translated body calls, defined once in the shader. */
const char *nv2a_vsh_glsl_helpers(void)
{
    return
        "uniform vec4 u_vc[192];\n"
        "int a0;\n"
        "vec4 vc_rel(int i) { return (i < 0 || i >= 192) ? u_vc[0] : u_vc[i]; }\n"
        "float vsh_inf(bool neg) { return uintBitsToFloat(neg ? 0xff800000u : 0x7f800000u); }\n"
        "vec4 vsh_rcp(float x) { return vec4(x == 0.0 ? vsh_inf(false) : 1.0 / x); }\n"
        "vec4 vsh_rcc(float x) {\n"
        "  float f = x == 0.0 ? vsh_inf((floatBitsToUint(x) >> 31) != 0u) : 1.0 / x;\n"
        "  if (abs(f) < 5.42101e-20) f = f < 0.0 ? -5.42101e-20 : 5.42101e-20;\n"
        "  if (abs(f) > 1.884467e19) f = f < 0.0 ? -1.884467e19 : 1.884467e19;\n"
        "  return vec4(f);\n"
        "}\n"
        "vec4 vsh_rsq(float x) { float a = abs(x); return vec4(a == 0.0 ? vsh_inf(false) : 1.0 / sqrt(a)); }\n"
        "vec4 vsh_exp(float x) { float fl = floor(x); return vec4(exp2(fl), x - fl, exp2(x), 1.0); }\n"
        "vec4 vsh_log(float x) {\n"
        "  float ax = abs(x);\n"
        "  if (ax == 0.0) return vec4(vsh_inf(true), 1.0, vsh_inf(true), 1.0);\n"
        "  float e = floor(log2(ax));\n"
        "  return vec4(e, ax / exp2(e), log2(ax), 1.0);\n"
        "}\n"
        "vec4 vsh_lit(vec4 c) {\n"
        "  float nl = c.x > 0.0 ? c.x : 0.0, nh = c.y > 0.0 ? c.y : 0.0;\n"
        "  float p = clamp(c.w, -127.9961, 127.9961);\n"
        "  float s = nh > 0.0 ? pow(nh, p) : (p == 0.0 ? 1.0 : (p > 0.0 ? 0.0 : vsh_inf(false)));\n"
        "  return vec4(1.0, nl, c.x > 0.0 ? s : 0.0, 1.0);\n"
        "}\n";
}

int nv2a_vsh_to_glsl(const uint32_t (*prog)[4], uint32_t start, uint32_t slots,
                     char *out, size_t cap, uint32_t *ninsn, uint32_t *writes_c,
                     uint32_t *inputs)
{
    static const char *const k_mac[16] = {
        "nop", "mov", "mul", "add", "mad", "dp3", "dph", "dp4",
        "dst", "min", "max", "slt", "sge", "arl", "mac14", "mac15"
    };
    static const char *const k_ilu[8] = {
        "nop", "mov", "rcp", "rcc", "rsq", "exp", "log", "lit"
    };
    Buf b = { out, cap, 0, 0, 0 };
    uint32_t s, n = 0, wc = 0;

    if (!cap)
        return 0;
    out[0] = 0;
    put(&b, "void nv2a_vsh(in vec4 v[16], out vec4 oPos, out vec4 O[13]) {\n");
    put(&b, "  vec4 R[12];\n  for (int k = 0; k < 12; k++) R[k] = vec4(0.0);\n");
    put(&b, "  oPos = vec4(0.0);\n  for (int k = 0; k < 13; k++) O[k] = vec4(0.0);\n");
    put(&b, "  O[3].w = 1.0; O[4].w = 1.0;\n");
    put(&b, "  O[9].w = 1.0; O[10].w = 1.0; O[11].w = 1.0; O[12].w = 1.0;\n");
    put(&b, "  a0 = 0;\n");
    for (s = start; s < slots; s++) {
        const uint32_t *ins = prog[s];
        uint32_t mac = field(ins, 1, 21, 4), ilu = field(ins, 1, 25, 3);
        uint32_t mac_mask = field(ins, 3, 24, 4), ilu_mask = field(ins, 3, 16, 4);
        uint32_t tdst = field(ins, 3, 20, 4);
        uint32_t omask = field(ins, 3, 12, 4), oaddr = field(ins, 3, 3, 8);

        n++;
        put(&b, "  { /* %u: %s %s */\n    vec4 A = ", s, k_mac[mac], k_ilu[ilu]);
        src(&b, ins, 0);
        put(&b, ", B = ");
        src(&b, ins, 1);
        put(&b, ", C = ");
        src(&b, ins, 2);
        put(&b, ";\n    vec4 m = vec4(0.0), i = vec4(0.0);\n");
        switch (mac) {
        case 0:  break;
        case 1:  put(&b, "    m = A;\n"); break;
        case 2:  put(&b, "    m = A * B;\n"); break;
        case 3:  put(&b, "    m = A + C;\n"); break;
        case 4:  put(&b, "    m = A * B + C;\n"); break;
        case 5:  put(&b, "    m = vec4(A.x * B.x + A.y * B.y + A.z * B.z);\n"); break;
        case 6:  put(&b, "    m = vec4(A.x * B.x + A.y * B.y + A.z * B.z + B.w);\n"); break;
        case 7:  put(&b, "    m = vec4(A.x * B.x + A.y * B.y + A.z * B.z + A.w * B.w);\n"); break;
        case 8:  put(&b, "    m = vec4(1.0, A.y * B.y, A.z, B.w);\n"); break;
        case 9:  put(&b, "    m = mix(B, A, lessThan(A, B));\n"); break;
        case 10: put(&b, "    m = mix(B, A, greaterThanEqual(A, B));\n"); break;
        case 11: put(&b, "    m = vec4(lessThan(A, B));\n"); break;
        case 12: put(&b, "    m = vec4(greaterThanEqual(A, B));\n"); break;
        case 13: put(&b, "    a0 = int(floor(A.x + 0.001)); m = A;\n"); break;
        default: put(&b, "    m = A;\n"); break;
        }
        switch (ilu) {
        case 0:  break;
        case 1:  put(&b, "    i = C;\n"); break;
        case 2:  put(&b, "    i = vsh_rcp(C.x);\n"); break;
        case 3:  put(&b, "    i = vsh_rcc(C.x);\n"); break;
        case 4:  put(&b, "    i = vsh_rsq(C.x);\n"); break;
        case 5:  put(&b, "    i = vsh_exp(C.x);\n"); break;
        case 6:  put(&b, "    i = vsh_log(C.x);\n"); break;
        default: put(&b, "    i = vsh_lit(C);\n"); break;
        }
        if (mac && mac != 13 && mac_mask && tdst <= 12) {
            char d[8];
            snprintf(d, sizeof d, tdst == 12 ? "oPos" : "R[%u]", tdst);
            put(&b, "   ");
            write_masked(&b, d, "m", mac_mask);
            put(&b, "\n");
        }
        if (ilu && ilu_mask) {
            uint32_t it = mac ? 1u : tdst;
            if (it <= 12) {
                char d[8];
                snprintf(d, sizeof d, it == 12 ? "oPos" : "R[%u]", it);
                put(&b, "   ");
                write_masked(&b, d, "i", ilu_mask);
                put(&b, "\n");
            }
        }
        if (omask) {
            const char *val = field(ins, 3, 2, 1) ? "i" : "m";
            if (!field(ins, 3, 11, 1)) {
                wc++;                         /* to c[]: see the header */
            } else if (oaddr == 0) {
                put(&b, "   ");
                write_masked(&b, "oPos", val, omask);
                put(&b, "\n");
            } else if (oaddr < 13) {
                char d[8];
                snprintf(d, sizeof d, "O[%u]", oaddr);
                put(&b, "   ");
                write_masked(&b, d, val, omask);
                put(&b, "\n");
            }
        }
        put(&b, "  }\n");
        if (field(ins, 3, 0, 1)) {
            put(&b, "}\n");
            if (ninsn)
                *ninsn = n;
            if (writes_c)
                *writes_c = wc;
            if (inputs)
                *inputs = b.inputs;
            return !b.over;
        }
    }
    return 0;                                 /* no FINAL */
}
