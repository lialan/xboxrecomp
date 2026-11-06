"""A guest jmp_buf re-armed from a nested frame: the longjmp goes to the frame
the buffer names when it is taken.

HOTD3's task scheduler (sub_0011B000) is re-entrant on one global jmp_buf: it
saves the buffer, re-arms it with setjmp, runs its tasks and copies the outer
contents back on the way out. The native slots were keyed by buffer address
alone, so the nested setjmp took over the outer scheduler's slot, and the
outer's next longjmp landed in the nested frame after it had returned.

Compiles recomp_setjmp_slot / recomp_guest_longjmp from xbox_memory_layout.c
with stand-ins for the globals they touch.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import pytest
from .c_fixture import compiler, command

SRC = Path(__file__).resolve().parents[2] / "src" / "kernel" / "xbox_memory_layout.c"


def _slot_code():
    text = SRC.read_text()
    start = text.index("#define RECOMP_JMPBUF_SLOTS")
    m = re.search(r"\nint recomp_guest_longjmp\(.*?\n}\n", text[start:], re.S)
    return text[start:start + m.end()]


def test_longjmp_goes_to_the_frame_the_buffer_names():
    cc = compiler()
    if not cc:
        pytest.skip('C compiler unavailable')
    code = '''#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define RECOMP_TLS
static uint8_t guest[0x10000];
static uintptr_t g_memory_offset;
static uint32_t g_ebx, g_edi, g_esi, g_esp, g_ebp, g_seh_ebp;
''' + _slot_code() + '''
#define BUF 0x100u
/* The guest setjmp: ebp, ebx, edi, esi, then esp at its entry (the call
 * site's esp less the return address). */
static void guest_setjmp(uint32_t site_esp, uint32_t ebx)
{
    uint32_t *b = (uint32_t *)(guest + BUF);
    b[0] = 0xEB0; b[1] = ebx; b[2] = 0xED1; b[3] = 0xE51; b[4] = site_esp - 4;
}
static int inner_ran;
static void nested_scheduler(void)
{
    uint8_t saved[0x40];
    memcpy(saved, guest + BUF, sizeof saved);           /* save the outer buffer */
    if (setjmp(*recomp_setjmp_slot(BUF, 0x0F00)) == 0) {
        guest_setjmp(0x0F00, 0x22);
        inner_ran = 1;
    }
    memcpy(guest + BUF, saved, sizeof saved);           /* copy it back */
}
int main(void)
{
    volatile int landed = 0;
    int v;
    g_memory_offset = (uintptr_t)guest;
    v = setjmp(*recomp_setjmp_slot(BUF, 0x1000));
    if (v == 0) {
        guest_setjmp(0x1000, 0x11);
        nested_scheduler();                              /* re-arms the same buffer */
        if (!recomp_guest_longjmp(BUF, 3)) return 2;     /* no slot for the outer frame */
        return 3;                                        /* longjmp returned */
    }
    landed = v;
    if (landed != 3 || !inner_ran) return 4;
    if (g_ebx != 0x11 || g_esp != 0x1000) return 5;     /* the outer frame's registers */
    /* A buffer whose frame has no slot is left to the guest's own longjmp. */
    guest_setjmp(0x0E00, 0x33);
    if (recomp_guest_longjmp(BUF, 1)) return 6;
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as temp:
        source = Path(temp) / 'test.c'
        source.write_text(code)
        exe = Path(temp) / 'test.exe'
        result = subprocess.run(command(cc, [source], exe), capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        assert result.returncode == 0, f"exit {result.returncode}" + result.stdout + result.stderr
