/* Indexed programmable transforms: repeated indices, hash collisions,
 * per-draw attribute/program changes, and context-write side effects. */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_backend.h"

#define MEM_BYTES 0x10000u
static uint8_t s_mem[MEM_BYTES];

/* ---- what the executor expects the runtime to provide ------------------ */
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)s_mem; }
uint32_t g_xbox_image_lo, g_xbox_image_hi;
uint32_t g_xbox_stack_base, g_xbox_stack_size;   /* #189 made the stack placement runtime state */
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0; }
void xbox_FramebufferWindowSet(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }
static uint32_t s_fence_va;                           /* the mirrored fence word, if any */
uint32_t xbox_Nv2aFenceWordVa(void) { return s_fence_va; }
int nv2a_pb_dma_walk(void) { return 1; }             /* RECOMP_PB_WALK=dma: 0x0310 is latched */
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_Nv2aFrameCounterFlip(void) {}
/* The flip hold (nv2a_flip_hold.c, linked in). No vblank moves here, so the
 * executor never holds a flip; the walk is never inside a pushbuffer CALL. */
int nv2a_pb_scan_in_call(void) { return 0; }
uint32_t xbox_VblankCount(void) { return 0; }
uint64_t xbox_HostNowNs(void) { return 0; }
void xbox_ThreadAcctName(const char *name) { (void)name; }
uint64_t xbox_VblankLastNs(void) { return 0; }
uint32_t xbox_Nv2aFrameCountersOwned(void) { return 0; }

static volatile uint32_t s_pgraph_400b10;
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset)
{
    return offset == 0x400B10u ? &s_pgraph_400b10 : NULL;
}

#include "nv2a_vsh_interp.h"
static unsigned executions;
int nv2a_vsh_run_reference(const float in[NV2A_VSH_INPUTS][4],Nv2aVshOutput *out);
int nv2a_vsh_run(const float in[NV2A_VSH_INPUTS][4],Nv2aVshOutput *out) {
    executions++;return nv2a_vsh_run_reference(in,out);
}
static Nv2aVertex captured[64];static unsigned captured_count;
static void draw(const Nv2aSurface *s,const Nv2aBatch *b) {
    (void)s;
    if(captured_count+b->count>64) abort();
    memcpy(captured+captured_count,b->vertices,b->count*sizeof *captured);captured_count+=b->count;
}
static const Nv2aBackend backend={.draw=draw};
extern void nv2a_pb_exec_method(uint32_t,uint32_t,uint32_t);
static void m(uint32_t method,uint32_t value) {nv2a_pb_exec_method(0,method,value);}
static const unsigned indices[]={0,1,2,0,2,3,256,257,258,0,1,2};
static void program(int add,int context) {
    uint32_t move[4]={0,1u<<21,0,0};
    move[1]|=0x1b;move[2]|=2u<<26; /* A=v0.xyzw */
    move[3]=(15u<<12)|(1u<<11)|1u;
    if(context) {
        uint32_t write[4];memcpy(write,move,sizeof write);write[3]=(15u<<12); /* c0 */
        nv2a_vsh_set_instruction(0,write);nv2a_vsh_set_instruction(1,move);
    } else {
        if(add) {move[1]=(3u<<21)|0x1b;move[2]|=0x1bu<<2;move[3]|=3u<<28;}
        nv2a_vsh_set_instruction(0,move);
    }
    nv2a_vsh_set_start_slot(0);nv2a_vsh_set_cxt_write(context);
}
static int run(float dx,float dy,int context) {
    captured_count=executions=0;uint32_t generation=nv2a_vsh_const_gen();
    m(0x17fc,5);
    for(unsigned i=0;i<sizeof indices/sizeof *indices;i++) m(0x1808,indices[i]);
    m(0x17fc,0);
    const char *enabled=getenv("RECOMP_VSH_VERTEX_CACHE");
    unsigned expected=(context || (enabled && strcmp(enabled,"1")))?12:10;
    if(executions!=expected) {fprintf(stderr,"executions=%u expected=%u\n",executions,expected);return 1;}
    if(captured_count!=12 || (context && nv2a_vsh_const_gen()-generation!=12)) {
        fprintf(stderr,"count=%u constant writes=%u\n",captured_count,nv2a_vsh_const_gen()-generation);return 1;
    }
    for(unsigned i=0;i<12;i++) {
        float *p=(float *)(s_mem+0x2000+indices[i]*16);
        if(captured[i].x!=p[0]+dx || captured[i].y!=p[1]+dy || captured[i].z!=p[2] || captured[i].rhw!=1) {
            fprintf(stderr,"vertex %u: %g,%g expected %g,%g\n",i,captured[i].x,captured[i].y,p[0]+dx,p[1]+dy);return 1;
        }
    }
    return 0;
}
int main(void) {
    nv2a_backend_register(&backend);
    m(0x208,8);m(0x200,32u<<16);m(0x204,32u<<16);m(0x20c,128);m(0x210,0x8000);
    m(0x1e94,2);m(0x1720,0x2000);m(0x1760,2|(4<<4)|(16<<8));
    const float points[][4]={{2,2,.5,1},{10,2,.5,1},{10,10,.5,1},{2,10,.5,1}};
    memcpy(s_mem+0x2000,points,sizeof points);
    memcpy(s_mem+0x2000+256*16,points,3*16);
    ((float *)(s_mem+0x2000+256*16))[0]=3;
    program(0,0);if(run(0,0,0)) return 1;
    ((float *)(s_mem+0x2000))[0]=4;if(run(0,0,0)) return 1;
    const float offset[4]={.5f,.25f,0,0};nv2a_vsh_set_constant(0,offset);
    program(1,0);if(run(.5f,.25f,0)) return 1;
    program(0,1);if(run(0,0,1)) return 1;
    puts("PASS: repeated/colliding indices, batch invalidation, program/constants and context-write multiplicity");return 0;
}
