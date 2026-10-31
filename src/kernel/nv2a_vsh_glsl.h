/**
 * NV2A vertex program -> GLSL (nv2a_vsh_glsl.c). For a back end that draws
 * vertex-program batches on the GPU (Nv2aRawGeometry, NV2A_RAW_PROGRAM).
 */
#ifndef XBOXRECOMP_NV2A_VSH_GLSL_H
#define XBOXRECOMP_NV2A_VSH_GLSL_H

#include <stddef.h>
#include <stdint.h>

/* The declarations and helper functions the translated body uses (the
 * constants as `uniform vec4 u_vc[192]`, a0, the ILU ops); once per shader,
 * before the body. */
const char *nv2a_vsh_glsl_helpers(void);

/* The program from slot `start` to its FINAL instruction as
 *   void nv2a_vsh(in vec4 v[16], out vec4 oPos, out vec4 O[13])
 * into out (cap bytes). Returns 1, with the instruction count and the number
 * of writes to c[] it ignored; 0 if there is no FINAL before `slots` or the
 * text does not fit. *inputs: the v[] the program reads, a bit each (a shader
 * declares only those: Apple's GL has 16 attributes in all). */
int nv2a_vsh_to_glsl(const uint32_t (*prog)[4], uint32_t start, uint32_t slots,
                     char *out, size_t cap, uint32_t *ninsn, uint32_t *writes_c,
                     uint32_t *inputs);

#endif
