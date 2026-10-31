#!/usr/bin/env python3
"""Compile actual raw-check snapshot and attribute fetch against guest RAM mock."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'src/kernel/nv2a_pb_exec.c').read_text()
a = source.index('static const Nv2aRawGeometry *s_check_geometry;')
fetch = source[a:source.index('static uint32_t surface_bpp(void);', a)]
a = source.index('static int raw_check_snapshot(')
snapshot = source[a:source.index('static void raw_check(', a)]
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define NV_VERTEX_ATTRS 16
typedef struct { uint32_t offset, stride, size, type; } VertexAttr;
typedef struct { const void *data; uint32_t bytes, stride, size, type; float value[4]; } Nv2aAttrib;
typedef struct { Nv2aAttrib attr[16]; uint32_t min_index, max_index; } Nv2aRawGeometry;
static struct { VertexAttr attr[16]; int inline_active; uint32_t inline_count, inline_buf[64]; } s_gpu;
static uint8_t guest[256];
static intptr_t xbox_GetMemoryOffset(void) { return (intptr_t)guest; }
static uint32_t attr_elem_bytes(const VertexAttr *a) { return a->size * 4; }
static int guest_span_ok(uint64_t at, uint32_t bytes) { return at + bytes <= sizeof guest; }
'''
checks = r'''
int main(void) {
    float font[8] = {278,455,0,1, 290,467,0,1};
    float full[8] = {0,0,0,1, 640,480,0,1};
    float out[4];
    Nv2aRawGeometry g = {0};
    s_gpu.attr[0] = (VertexAttr){64,16,4,2};
    memcpy(guest + 64, font, sizeof font);
    g.attr[0].data = guest + 64;
    g.attr[0].bytes = sizeof font;
    g.attr[0].stride = 16;
    g.attr[0].size = 4;
    g.attr[0].type = 2;
    g.max_index = 1;
    assert(raw_check_snapshot(&g));
    memcpy(guest + 64, full, sizeof full); /* producer reuses RAM during GL drain */
    assert(fetch_attr(&s_gpu.attr[0], 0, out) && out[0] == 0);
    s_check_geometry = &g;
    assert(fetch_attr(&s_gpu.attr[0], 0, out) && !memcmp(out, font, 16));
    assert(fetch_attr(&s_gpu.attr[0], 1, out) && !memcmp(out, font + 4, 16));
    assert(!memcmp(g.attr[0].data, font, sizeof font)); /* GPU reads this same snapshot */
    assert(!fetch_attr(&s_gpu.attr[0], 2, out));
    s_check_geometry = NULL;
    assert(fetch_attr(&s_gpu.attr[0], 1, out) && !memcmp(out, full + 4, 16));
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='raw-check-') as temp:
    temp = Path(temp)
    (temp / 'test.c').write_text(harness + fetch + snapshot + checks)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(temp / 'test.c'),
                    '-o', str(temp / 'test')], check=True)
    subprocess.run([str(temp / 'test')], check=True)
print('checked CPU and GPU attributes remain identical after guest overwrite: PASS')
