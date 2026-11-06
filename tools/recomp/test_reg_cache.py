"""Tests for --reg-cache (reg_cache.py): which registers become locals, and
where the thread-local block is brought up to date."""

from tools.recomp.reg_cache import reg_cache_function

FUNC = '''void sub_00001000(void)
{
    int _flags = 0;
loc_00001000: ;
    eax = MEM32(esp + 4);
    fp_top() = 1.0; fp_pop();
    if (eax == 0) { esp += 4; return; /* ret */ }
    PUSH32(esp, 0x1005u); RECOMP_ABI_CALL(0x00002000u, sub_00002000);
    recomp_debug_service(1, eax);
    g_seh_ebp = ebp; sub_00003000(); return;
}
'''


def test_only_named_registers_are_cached():
    out = reg_cache_function(FUNC)
    assert "uint32_t rc_eax = g_regs.r_eax;" in out
    assert "uint32_t rc_esp = g_regs.r_esp;" in out
    assert "rc_ecx" not in out and "RC_ECX" not in out
    assert out.index('#include "recomp_rc_on.h"') < out.index("loc_00001000")


def test_block_written_back_at_returns_and_direct_calls():
    out = reg_cache_function(FUNC)
    assert out.count("{ RECOMP_RC_STORE(); return; }") == 2
    assert "{ RECOMP_RC_STORE(); sub_00003000(); RECOMP_RC_LOAD(); }" in out
    assert "{ RECOMP_RC_STORE(); recomp_debug_service(1, eax); RECOMP_RC_LOAD(); }" in out
    # the macro call is wrapped by the macro itself (recomp_rc_on.h)
    assert "RECOMP_ABI_CALL(0x00002000u, sub_00002000);" in out
    # the end: written back, names restored, before the closing brace
    tail = out.rstrip()
    assert tail.endswith('#include "recomp_rc_off.h"\n}')


def test_fpu_stack_macros_are_not_calls():
    out = reg_cache_function(FUNC)
    assert "fp_pop();" in out and "RECOMP_RC_STORE(); fp_pop" not in out


SETJMP_FUNC = '''void sub_00006000(void)
{
loc_00006000: ;
    esi = MEM32(esp + 4);
    { int _sjv = setjmp(*recomp_setjmp_slot(MEM32(esp), esp)); if (_sjv) { ebp = g_seh_ebp; eax = (uint32_t)_sjv; } else { PUSH32(esp, 0x00006010u); sub_00007000(); } } /* setjmp 0x00007000 */
    MEM32(esi) = eax; return;
}
'''


def test_registers_reloaded_where_a_longjmp_lands():
    # recomp_guest_longjmp restores ebx/esi/edi/esp into g_regs; the locals
    # that cache them are indeterminate after the native longjmp, so the
    # landing branch reloads them all, then takes eax from the jump.
    out = reg_cache_function(SETJMP_FUNC)
    assert "if (_sjv) { RECOMP_RC_LOAD(); ebp = g_seh_ebp; eax = (uint32_t)_sjv; }" in out
    assert "{ RECOMP_RC_STORE(); sub_00007000(); RECOMP_RC_LOAD(); }" in out


def test_function_naming_no_register_is_unchanged():
    code = "void sub_00004000(void)\n{\n    MEM32(0x100) = 1;\n    sub_00005000(); return;\n}\n"
    assert reg_cache_function(code) == code
