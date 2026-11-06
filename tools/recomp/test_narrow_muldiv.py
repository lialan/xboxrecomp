"""One-operand MUL, IMUL, DIV and IDIV work at their operand's width.

An 8-bit operand multiplies AL into AX, or divides AX into AL (quotient) and
AH (remainder); a 16-bit one works on DX:AX; only the 32-bit form uses
EDX:EAX. Lifting "imul bl" as the 32-bit form overwrote EDX with the high
half, and HOTD3's start-up then stored through the clobbered pointer.
"""
from pathlib import Path
import subprocess
import tempfile
import pytest
from . import config
from .translator import FunctionTranslator
from .c_fixture import compiler, command

# mnemonic, operand width, encoding (operand bl, cx or ecx), then ret
FORMS = [
    ("imul", 8, "f6eb"), ("mul", 8, "f6e3"), ("div", 8, "f6f3"), ("idiv", 8, "f6fb"),
    ("imul", 16, "66f7e9"), ("mul", 16, "66f7e1"), ("div", 16, "66f7f1"), ("idiv", 16, "66f7f9"),
    ("imul", 32, "f7e9"), ("mul", 32, "f7e1"), ("div", 32, "f7f1"), ("idiv", 32, "f7f9"),
]


def test_narrow_muldiv_compiled():
    cc = compiler()
    if not cc:
        pytest.skip('C compiler unavailable')
    code = '''#include <stdint.h>
#include <stdio.h>
static uint32_t eax, ebx, ecx, edx, esp;
#define LO8(v) ((uint8_t)(v))
#define HI8(v) ((uint8_t)((v) >> 8))
#define LO16(v) ((uint16_t)(v))
#define ZX8(v) ((uint32_t)(uint8_t)(v))
#define SX8(v) ((uint32_t)(int32_t)(int8_t)(v))
#define SX16(v) ((uint32_t)(int32_t)(int16_t)(v))
#define SET_LO8(v,x) ((v)=((v)&0xffffff00u)|(uint8_t)(x))
#define SET_HI8(v,x) ((v)=((v)&0xffff00ffu)|((uint32_t)(uint8_t)(x)<<8))
#define SET_LO16(v,x) ((v)=((v)&0xffff0000u)|(uint16_t)(x))
#define RECOMP_PARITY8(v) ((0x9669u >> (((v) ^ ((v) >> 4)) & 15u)) & 1u)
'''
    names = []
    base = 0x10000
    for i, (m, w, enc) in enumerate(FORMS):
        image = bytes.fromhex(enc + "c3")
        config._install([config.Section('.text', base, len(image), 0, len(image), True)],
                        entry_point=base, kernel_thunk_addr=base, origin='narrow-muldiv')
        db = {base: {'start': hex(base), 'end': base + len(image), '_addr': base, 'size': len(image)}}
        name = f'fixture_{m}_{w}'
        names.append(name)
        code += FunctionTranslator(image, db).translate_function(base, db[base]).replace('sub_00010000', name)
    code += 'static void (*fixtures[%d])(void)={%s};\n' % (len(names), ','.join(names))
    code += '''
/* x86 reference: form f (op = i&3: imul mul div idiv; width 8 << (i>>2)) */
static int reference(unsigned f, uint32_t a, uint32_t d, uint32_t s, uint32_t *ra, uint32_t *rd) {
    unsigned op = f & 3, w = 8u << (f >> 2);
    *ra = a; *rd = d;
    if (w == 8) {
        uint16_t ax = (uint16_t)a; uint8_t b = (uint8_t)s;
        uint16_t r;
        if (op == 0) r = (uint16_t)((int16_t)(int8_t)ax * (int16_t)(int8_t)b);
        else if (op == 1) r = (uint16_t)((uint8_t)ax * b);
        else if (op == 2) { if (!b || ax / b > 0xff) return 0; r = (uint16_t)((ax % b) << 8 | (ax / b)); }
        else { int16_t n = (int16_t)ax, q; int8_t v = (int8_t)b;
               if (!v) return 0; q = (int16_t)(n / v); if (q < -128 || q > 127) return 0;
               r = (uint16_t)(((uint8_t)(int8_t)(n % v)) << 8 | (uint8_t)(int8_t)q); }
        *ra = (a & 0xffff0000u) | r;
    } else if (w == 16) {
        uint16_t ax = (uint16_t)a, dx = (uint16_t)d, v = (uint16_t)s; uint32_t lo, hi;
        if (op == 0) { int32_t p = (int32_t)(int16_t)ax * (int16_t)v; lo = (uint16_t)p; hi = (uint16_t)((uint32_t)p >> 16); }
        else if (op == 1) { uint32_t p = (uint32_t)ax * v; lo = (uint16_t)p; hi = (uint16_t)(p >> 16); }
        else if (op == 2) { uint32_t n = (uint32_t)dx << 16 | ax; if (!v || n / v > 0xffff) return 0; lo = n / v; hi = n % v; }
        else { int32_t n = (int32_t)((uint32_t)dx << 16 | ax), q; if (!v) return 0;
               q = n / (int16_t)v; if (q < -32768 || q > 32767) return 0;
               lo = (uint16_t)q; hi = (uint16_t)(n % (int16_t)v); }
        *ra = (a & 0xffff0000u) | (lo & 0xffff); *rd = (d & 0xffff0000u) | (hi & 0xffff);
    } else {
        if (op == 0) { int64_t p = (int64_t)(int32_t)a * (int32_t)s; *ra = (uint32_t)p; *rd = (uint32_t)((uint64_t)p >> 32); }
        else if (op == 1) { uint64_t p = (uint64_t)a * s; *ra = (uint32_t)p; *rd = (uint32_t)(p >> 32); }
        else if (op == 2) { uint64_t n = (uint64_t)d << 32 | a; if (!s || n / s > 0xffffffffu) return 0; *ra = (uint32_t)(n / s); *rd = (uint32_t)(n % s); }
        else { int64_t n = (int64_t)((uint64_t)d << 32 | a), q; if (!s) return 0;
               q = n / (int32_t)s; if (q < INT32_MIN || q > INT32_MAX) return 0;
               *ra = (uint32_t)q; *rd = (uint32_t)(n % (int32_t)s); }
    }
    return 1;
}
int main(void) {
    const uint32_t as[] = {0, 1, 3, 0x7f, 0x80, 0xff, 0x1234, 0xabcd7f, 0x8001, 0xdeadbe02, 0x7fffffff, 0x80000000, 0xfffffffd};
    const uint32_t ds[] = {0, 1, 0x7f, 0xffffffff, 0xcafe0002};
    const uint32_t ss[] = {1, 2, 3, 0x7f, 0x80, 0xfd, 0x1203, 0x8000, 0xff7f, 0x12345681, 0xfffffffe};
    unsigned checked = 0;
    for (unsigned f = 0; f < sizeof fixtures / sizeof fixtures[0]; f++)
        for (unsigned i = 0; i < sizeof as / 4; i++)
            for (unsigned j = 0; j < sizeof ds / 4; j++)
                for (unsigned k = 0; k < sizeof ss / 4; k++) {
                    uint32_t ra, rd, src = ss[k], sb = (f >> 2) == 0 ? 0x5a5a5a00u | (src & 0xff) : 0x5a5a5a5au;
                    uint32_t sc = (f >> 2) == 0 ? 0xc3c3c3c3u : (f >> 2) == 1 ? 0xc3c30000u | (src & 0xffff) : src;
                    if (!reference(f, as[i], ds[j], (f >> 2) == 0 ? sb : sc, &ra, &rd)) continue;
                    eax = as[i]; edx = ds[j]; ebx = sb; ecx = sc; esp = 1024;
                    fixtures[f]();
                    checked++;
                    if (eax != ra || edx != rd || ebx != sb || ecx != sc || esp != 1028) {
                        fprintf(stderr, "form %u eax=%08x edx=%08x src=%08x: got eax=%08x edx=%08x, want eax=%08x edx=%08x\\n",
                                f, as[i], ds[j], (f >> 2) == 0 ? sb : sc, eax, edx, ra, rd);
                        return 1;
                    }
                }
    return checked > 500 ? 0 : 2;
}
'''
    with tempfile.TemporaryDirectory() as temp:
        source = Path(temp) / 'test.c'
        source.write_text(code)
        exe = Path(temp) / 'test.exe'
        result = subprocess.run(command(cc, [source], exe), capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
