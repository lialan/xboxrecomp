"""--reg-cache: a lifted function's general-purpose guest registers in C
locals (templates/runtime/recomp_rc_on.h). A pass over the C text the
translator produced for one function; kept apart so it can be tested
without the disassembler."""
import re
# What is cached, and what is not and why:
# - ebp and the arithmetic flags need no cache: every lifted function already
#   holds ebp in a C local (`uint32_t ebp`, handed over through g_ebp /
#   g_seh_ebp at calls) and computes flags into locals (_fa, _fb, _fas, _fbs,
#   _flags); nothing of either is read from the thread-local block in a
#   function body.
# - The x87 stack top (g_fp_top) and last compare (g_fp_cmp) were tried as
#   locals the same way and made the code larger: in VC3's most x87-heavy
#   file (recomp_0005.c, Quest flags) 87,826 instructions with only the
#   general-purpose registers cached, 88,141 with the stack top too and
#   89,773 with both -- the write-back and reload at every call cost more
#   than the reloads they save. The condition codes (g_fp_cc) and the
#   direction flag (g_df) cannot be: header functions write them
#   (recomp_fp_trig_in_range, recomp_fprem, recomp_pushfd/popfd).
_RC_REGS = ("eax", "ecx", "edx", "ebx", "esi", "edi", "esp")
# A direct call to a lifted function (the tail calls and fall-throughs);
# not the FPU stack macros (fp_pop(), fp_top()), which look the same.
_RC_DIRECT_CALL = re.compile(r'(?<![\w.>])(sub_[0-9A-Fa-f]{8}\w*)\(\);')
_RC_RETURN = re.compile(r'\breturn;')


def reg_cache_function(code):
    """--reg-cache: the function's general-purpose guest registers in locals
    (recomp_rc_on.h), written back before every call and return, reloaded
    after every call. The calls through RECOMP_ABI_CALL (and so every
    RECOMP_ICALL* and RECOMP_ITAIL) are wrapped by the macro itself; this
    wraps the rest: direct calls `name();` (the tail calls and fall-throughs
    into the next function), recomp_debug_service, and `return;`."""
    head = code.find("(void)\n{\n")
    if head < 0:
        return code
    body_start = head + len("(void)\n{\n")
    end = code.rstrip().rfind("}")
    body = code[body_start:end]
    used = [r for r in _RC_REGS
            if re.search(r'\b(?:g_)?%s\b' % r, body)]
    if not used:
        return code
    body = _RC_RETURN.sub("{ RECOMP_RC_STORE(); return; }", body)
    body = _RC_DIRECT_CALL.sub(
        lambda m: "{ RECOMP_RC_STORE(); %s(); RECOMP_RC_LOAD(); }" % m.group(1), body)
    body = re.sub(r'(recomp_debug_service\([^;]*\);)',
                  r'{ RECOMP_RC_STORE(); \1 RECOMP_RC_LOAD(); }', body)
    # Where a guest longjmp lands (the lifter's native setjmp): the runtime
    # put the jmp_buf's ebx/esi/edi/esp into g_regs, and the locals caching
    # them are indeterminate after the native longjmp -- reload them all
    # before eax takes the jump's value. Without it HOTD3's task loop
    # (sub_0011B000) resumed on stale locals and stored through garbage.
    body = body.replace("if (_sjv) { ebp = g_seh_ebp;",
                        "if (_sjv) { RECOMP_RC_LOAD(); ebp = g_seh_ebp;")
    enter = ["    uint32_t rc_%s = g_regs.r_%s;" % (r, r) for r in used]
    enter += ["#define RC_%s" % r.upper() for r in used]
    enter.append('#include "recomp_rc_on.h"')
    leave = ["    RECOMP_RC_STORE();   /* falling off the end */",
             '#include "recomp_rc_off.h"']
    return (code[:body_start] + "\n".join(enter) + "\n" + body.rstrip("\n") + "\n"
            + "\n".join(leave) + "\n" + code[end:])

