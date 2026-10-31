/*
 * NV2A render back-end interface.
 *
 * The pushbuffer executor (nv2a_pb_exec.c) decodes what a title asks the GPU
 * to do. By default it carries that out itself, on the CPU, into guest memory.
 * A game project can instead register a back end -- a D3D11 renderer, say --
 * and the executor hands it the work in already-decoded form:
 *
 *   surface state + clears, triangles (transformed to surface pixels, with
 *   per-vertex colour and texel-space UVs), the texture each batch samples,
 *   and the flip that ends a frame.
 *
 * Everything is called on the NV2A poll thread, one call at a time, so a back
 * end may create its window and device lazily on the first call and pump its
 * window messages from flip().
 *
 * Fixed-function, pre-transformed and vertex-program batches all arrive the
 * same way: the executor transforms fixed-function vertices itself, runs
 * vertex programs on the CPU (nv2a_vsh_interp.c) and clips at the near plane,
 * so a back end only ever sees surface pixels. Blend, depth, alpha, cull and
 * stencil state arrive in Nv2aRenderState. The exception is a back end with
 * draw_raw(), which is offered fixed-function batches untransformed
 * (Nv2aRawGeometry, below) under RECOMP_GPU_XF.
 *
 * ponytail: register-combiner state and texture stages 1-3 are not passed on:
 * a back end sees stage 0 and the diffuse colour. Extend Nv2aBatch when that
 * matters rather than adding callbacks.
 */
#ifndef NV2A_BACKEND_H
#define NV2A_BACKEND_H

#include <stdint.h>
#include "nv2a_combiner.h"   /* Nv2aCombiner, for Nv2aBatch.rc */

/* The colour surface being drawn into, in real (anti-aliased) pixels.
 * aa_sx/aa_sy give the anti-aliasing factor, so width/aa_sx is the logical
 * size the title thinks it renders at (e.g. 640x480). */
typedef struct {
    uint32_t color_va;          /* guest address of pixel (0,0) */
    uint32_t width, height;     /* clip rectangle size, real pixels */
    uint32_t pitch;             /* bytes per row */
    uint32_t bytes_per_pixel;   /* 2 or 4 */
    uint32_t aa_sx, aa_sy;      /* 1 or 2 each */
} Nv2aSurface;

/* A texture as the title programmed it. uv in Nv2aVertex are in texels. */
typedef struct {
    uint32_t offset;            /* guest address of texel (0,0) */
    uint32_t width, height;
    uint32_t pitch;             /* linear formats only */
    uint32_t color;             /* NV097 colour-format code */
    uint32_t addr_u, addr_v;    /* NV097 wrap mode per axis (1 wrap, 3 clamp) */
    /* Mip levels stored after level 0 (the format word's MIPMAP_LEVELS; 1 for
     * a linear texture) and the raw SET_TEXTURE_FILTER word (MIN in bits
     * 16-23). A back end that ignores them samples level 0. */
    uint32_t levels, filter;
    /* A cube map (the format word's CUBEMAP_ENABLE): six faces, +X -X +Y -Y
     * +Z -Z, each a full mip chain, each starting on a 128-byte boundary. */
    uint32_t cube;
} Nv2aTexture;

typedef struct {
    float    x, y, z;           /* surface pixels; z as the title produced it */
    float    rhw;               /* 1/w, 1 for pre-transformed batches */
    uint32_t diffuse;           /* 0xAARRGGBB */
    float    u, v;              /* texels */
} Nv2aVertex;

/* Render state as the title set it, in NV2A's own (OpenGL) enum values:
 * blend factors GL_ZERO/GL_ONE/0x300..0x308/0x8001..0x8004, equations
 * GL_FUNC_ADD 0x8006 etc., compare functions GL_NEVER 0x200 .. GL_ALWAYS 0x207.
 * color_mask uses NV2A's bytes: A 0x01000000, R 0x00010000, G 0x100, B 0x1. */
typedef struct {
    uint32_t blend_enable, blend_src, blend_dst, blend_eq, blend_color;
    uint32_t alpha_test_enable, alpha_func, alpha_ref;   /* ref 0..255 */
    uint32_t depth_test_enable, depth_func, depth_write;
    uint32_t color_mask;
    uint32_t cull_enable, cull_face, front_face;
    uint32_t zeta_va;                                    /* 0: no depth surface */
    float    depth_min, depth_max;                       /* SET_CLIP_MIN/MAX */
    /* SET_STENCIL_*: compare function as above; ops GL_KEEP 0x1E00, GL_ZERO 0,
     * GL_REPLACE 0x1E01, GL_INCR 0x1E02, GL_DECR 0x1E03, GL_INVERT 0x150A,
     * GL_INCR_WRAP 0x8507, GL_DECR_WRAP 0x8508. Reset values GL_ALWAYS, masks
     * 0xFF, GL_KEEP. A title can confine a pass to the pixels an earlier pass
     * marked (a full-screen quad that only marked pixels pass); a back end
     * that ignores these draws that pass over the whole surface. */
    uint32_t stencil_enable, stencil_func, stencil_ref, stencil_func_mask;
    uint32_t stencil_write_mask, stencil_fail, stencil_zfail, stencil_zpass;
} Nv2aRenderState;

/* What the register combiners read per vertex, beyond Nv2aVertex: for a back
 * end that evaluates them (see Nv2aBatch.rc). Texture coordinates are the
 * vertex program's outputs, per stage, as (s, t, r, q): normalised for
 * swizzled and DXT textures, texels for linear ones -- the same units the
 * software rasteriser samples with. */
typedef struct {
    uint32_t specular;          /* 0xAARRGGBB, the combiners' v1 */
    float    fog;               /* fog factor 0..1 (1: no fog) */
    float    tex[4][4];
} Nv2aVertexEx;

/* ---- Raw geometry: a back end that transforms vertices itself -------------
 *
 * Transforming every vertex on the CPU, once per index and with no vertex
 * cache, then expanding each triangle into Nv2aVertex + Nv2aVertexEx, is most
 * of the executor thread's time in a 3D scene. A back end with a draw_raw()
 * entry point is offered the batch untransformed instead: the attribute
 * streams as the title laid them out, the index list, and the transform state
 * with every executor quirk and switch already resolved (Nv2aFixedState), so
 * the back end's shader is pure arithmetic. It must produce what the CPU path
 * would: the same screen position, colours and texture coordinates per
 * vertex, clipped at w = 1e-3 and (near_z) at the near plane.
 *
 * Fixed function (NV2A_RAW_FIXED) carries nearly all of a 3D scene's
 * vertices. Vertex-program batches (NV2A_RAW_PROGRAM) come with the program
 * and its constants instead of Nv2aFixedState; nv2a_vsh_glsl.h translates
 * the program. */

#define NV2A_RAW_PROGRAM 1
#define NV2A_RAW_FIXED   2

/* One NV2A vertex attribute as the title set it. */
typedef struct {
    const void *data;       /* host pointer to element min_index; NULL: constant */
    uint32_t guest_va;      /* guest address of the same (0 when constant) */
    uint32_t type, size, stride;   /* NV097 encodings (type nibble, 1-4); bytes */
    uint32_t bytes;         /* readable span from data: (max-min)*stride + element */
    float    value[4];      /* the value when data is NULL: 0,0,0,1 */
} Nv2aAttrib;

/* Fixed-function transform, lighting and texgen, as the executor's CPU path
 * (ffp_vertex) evaluates it. Matrices are as the methods deliver them:
 * row-major, result[i] = sum_j m[4i+j] * v[j]. */
typedef struct {
    float composite[16];          /* clip = composite * pos when use_composite */
    float mv[4][16];              /* model-view k (0x0480 + 0x40k) */
    float proj[16];               /* 0x0440, or constants 0-3 when that is empty */
    float texmat[4][16];
    float c59[4];                 /* viewport offset */
    /* SET_SKIN_MODE as the title set it (0 when out of range): the blend
     * weights for the eye-space position and normal texgen reads, and -- unless
     * use_composite -- for the clip position. */
    uint32_t skin_mode;
    uint32_t use_composite;       /* unskinned (or skinning off): composite */
    uint32_t lighting;            /* lighting on and a usable normal */
    uint32_t light_kind[8];       /* 0 off, 1 infinite, 2+ local */
    uint32_t colmat_amb, colmat_dif;   /* COLOR_MATERIAL takes them from d0 */
    uint32_t flip_normals;        /* RECOMP_FFP_FLIP_NORMALS */
    float    scene_amb[4];
    float    mat_alpha;
    /* Per light, five vec4s: ambient.rgb range | diffuse.rgb - | direction - |
     * position - | attenuation 0,1,2 - */
    float    light[8][20];
    uint32_t texgen[4][4];        /* SET_TEXGEN_S/T/R/Q per stage */
    uint32_t texgen_on[4];        /* a mode texgen implements, and a normal */
    uint32_t texmat_on[4];
    /* Attribute slots, as the executor's heuristics picked them
     * (color_attr, texcoord_attr). Absent streams are constants in attr[]. */
    uint32_t color_slot, tex0_slot;
    uint32_t has_normal, has_color;   /* 0: no normal stream / white diffuse */
} Nv2aFixedState;

typedef struct {
    /* NV_PRIM_TRIANGLES (5), _TRIANGLE_STRIP (6) or _TRIANGLE_FAN (7); quads
     * arrive already split into triangles, points and lines never. */
    uint32_t prim;
    const uint16_t *indices;      /* rebased: 0 is min_index */
    uint32_t index_count, min_index, max_index;
    Nv2aAttrib attr[16];
    uint32_t kind;                /* NV2A_RAW_FIXED or NV2A_RAW_PROGRAM */
    const Nv2aFixedState *ff;     /* NV2A_RAW_FIXED */
    /* NV2A_RAW_PROGRAM: program memory, the program's start slot, length to
     * its FINAL and identity (a hash of its words); the constants c[0..191]
     * and their change count. Inputs v0..v15 are attr[0..15] (an attribute
     * with no array is a constant: its current SET_VERTEX_DATA value). The
     * shader's oPos is the interpreter's: x y z in surface pixels before the
     * anti-aliasing scale, w the clip w (+0 read as 1). w0 is what the CPU
     * gets for the first vertex -- 1 for a screen-space batch -- for the
     * back end's decisions that look at the transformed vertices. */
    const uint32_t (*program)[4];
    uint32_t program_start, program_len;
    uint64_t program_hash;
    const float (*constants)[4];
    uint32_t const_gen;
    float w0;
    /* Screen space: surface pixels = (clip.xy / w + c59.xy) * aa. */
    float aa_sx, aa_sy;
    float vp_off_z;               /* c59.z: the near plane is z = vp_off_z */
    uint32_t near_z;              /* clip at it (RECOMP_CLIP_NEAR_Z) */
    uint32_t fog_enable, fog_mode;  /* fog_factor() of the fog coordinate */
    float fog_param[2];
    float uv_scale0[2];           /* stage 0 in texels: (w, h); else (1, 1) */
    /* RECOMP_GPU_XF_CHECK: when set, also write each vertex min..max as the
     * shader computed it, 29 floats: screen x y z w, d0 rgba, d1 rgba,
     * fog factor, then T0-T3 (s t r q). */
    float *check_out;
} Nv2aRawGeometry;

typedef struct {
    const Nv2aVertex  *vertices;    /* triangle list: count is a multiple of 3 */
    uint32_t           count;
    const Nv2aTexture *texture;     /* NULL: untextured */
    const Nv2aRenderState *state;
    /* Register combiners, for batches the software rasteriser would shade
     * through them; NULL means texture * diffuse. When set, ext holds count
     * entries parallel to vertices, stage[i] the texture stage i samples
     * (NULL: stage off or unusable) and fog_color the fog colour (ARGB).
     * Stage modes are in rc->stage_program, as SHADER_STAGE_PROGRAM. */
    const Nv2aCombiner *rc;
    const Nv2aVertexEx *ext;
    const Nv2aTexture  *stage[4];
    uint32_t            fog_color;
    /* Set while the title counts visible pixels (SET_ZPASS_PIXEL_COUNT_ENABLE):
     * the back end adds the pixels of this batch that pass the depth test.
     * The count answers GET_REPORT, the visibility test titles cull with;
     * left at 0, everything they test is "hidden" and never drawn. */
    uint32_t           *zpass_out;
    /* draw_raw() only: the untransformed batch. vertices and ext are NULL and
     * count is 0; texture, state, rc, stage, fog_color and zpass_out mean the
     * same as above. */
    const Nv2aRawGeometry *raw;
} Nv2aBatch;

#define NV2A_RAW_CHECK_FLOATS 29

typedef struct {
    /* flags: CLEAR_SURFACE bits (Z 0x1, stencil 0x2, colour 0xF0).
     * zstencil: SET_ZSTENCIL_CLEAR_VALUE (depth in the top 24 bits for Z24S8). */
    void (*clear)(const Nv2aSurface *s, const Nv2aRenderState *rs,
                  uint32_t flags, uint32_t argb, uint32_t zstencil);
    void (*draw)(const Nv2aSurface *s, const Nv2aBatch *b);
    void (*flip)(void);
    /* Optional: draw b->raw, transforming on the back end. Returns 0 to
     * decline, and the executor then transforms the batch itself. A back end
     * initialised as { clear, draw, flip } leaves it NULL. */
    int  (*draw_raw)(const Nv2aSurface *s, const Nv2aBatch *b);
} Nv2aBackend;

/* Register (or, with NULL, remove) the back end. Call before the title starts
 * submitting work; the executor must also be enabled (RECOMP_PB_EXEC). */
void nv2a_backend_register(const Nv2aBackend *backend);

/* From flip(): the surface the title just finished, which the present shows
 * (0 if unknown). A back end need only resolve this one into guest memory. */
uint32_t nv2a_backend_presented_va(void);

/* Decode a whole texture to 0xAARRGGBB, row-major, width*height entries.
 * Handles every format the executor can sample (swizzled, linear, DXT).
 * Returns 0 if the format is not supported. */
int nv2a_backend_decode_texture(const Nv2aTexture *tex, uint32_t *argb_out);

/* Where BACK_END_WRITE_SEMAPHORE_RELEASE (0x1D70) values land: the guest VA
 * of the semaphore the title reads GPU progress from (for XDK D3D, the
 * pointer at device+0x30). The executor writes each release there once it
 * has executed everything before it -- which is
 * what lets D3D's fence waits and ring-space checks see real progress.
 *
 * ponytail: the title supplies the address instead of the executor resolving
 * the semaphore context DMA object through RAMIN. */
void nv2a_pb_set_semaphore_target(uint32_t guest_va);

#endif /* NV2A_BACKEND_H */
