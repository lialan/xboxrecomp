"""--reg-cache: a lifted function's general-purpose guest registers in C
locals (templates/runtime/recomp_rc_on.h). A pass over the C text the
translator produced for one function; kept apart so it can be tested
without the disassembler."""
import re
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
    enter = ["    uint32_t rc_%s = g_regs.r_%s;" % (r, r) for r in used]
    enter += ["#define RC_%s" % r.upper() for r in used]
    enter.append('#include "recomp_rc_on.h"')
    leave = ["    RECOMP_RC_STORE();   /* falling off the end */",
             '#include "recomp_rc_off.h"']
    return (code[:body_start] + "\n".join(enter) + "\n" + body.rstrip("\n") + "\n"
            + "\n".join(leave) + "\n" + code[end:])

