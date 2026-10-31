"""--ordered-mem: spell a lifted function's guest memory accesses by kind
-- load or store, through the stack or not -- for recomp_types.h
(RECOMP_MEM_SPLIT). A pass over the C text the translator produced for one
function, like reg_cache; kept apart so it can be tested without the
disassembler.

On an arm64 host every guest access is ordered (LDAR/STLR), because titles
hand work between threads through plain memory. That costs (VC3's game
thread: 89 -> 106 ms/s on an M4 Pro), and this pass spells each access by
kind so that the header can relax one kind without touching the others:

  MEMn(addr) = v;       ->  STn(addr, v);          store
  MEMn(esp + 8) = v;    ->  STn_STK(esp + 8, v);   store to the stack
  MEMn(addr)            ->  MEMn(addr)             load
  MEMn(esp + 8)         ->  MEMn_STK(esp + 8)      load from the stack

(SMEMn likewise.) An access is the stack's when its address starts from
esp, or from ebp in a function that owns a frame (push ebp; mov/lea ebp
from esp, or __SEH_prolog's); anywhere else ebp may be a plain register
holding any pointer. What each kind compiles to is recomp_types.h's: by
default all of them ordered, exactly as without the pass, because neither
relaxation tried on VC3 held -- plain stack accesses, and TSO's LDAPR for
the loads, each brought its attract stall back (see the header).

A store's value runs to the `;` that ends the statement at bracket depth
0; addresses and values are rewritten recursively, since either can hold
loads of their own.
"""
import re

_ACCESS = re.compile(r'(?<![\w.>])(S?MEM)(8|16|32)\(')
_STACK_ESP = re.compile(r'^\s*\(*\s*(?:g_)?esp\b')
_STACK_EBP = re.compile(r'^\s*\(*\s*ebp\b')


def _match_paren(s, i):
    """Index of the ')' closing the '(' at s[i], or -1."""
    depth = 0
    j = i
    n = len(s)
    while j < n:
        c = s[j]
        if c in "\"'":
            q = c
            j += 1
            while j < n and s[j] != q:
                j += 2 if s[j] == "\\" else 1
        elif c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                return j
        j += 1
    return -1


def _statement_end(s, i):
    """Index of the ';' ending the expression that starts at s[i] (depth 0),
    or -1."""
    depth = 0
    j = i
    n = len(s)
    while j < n:
        c = s[j]
        if c in "\"'":
            q = c
            j += 1
            while j < n and s[j] != q:
                j += 2 if s[j] == "\\" else 1
        elif c in "([{":
            depth += 1
        elif c in ")]}":
            if depth == 0:
                return -1          # the expression ends inside a bracket
            depth -= 1
        elif c == ";" and depth == 0:
            return j
        j += 1
    return -1


def _is_stack(addr, frame_ebp):
    if _STACK_ESP.match(addr):
        return True
    return bool(frame_ebp and _STACK_EBP.match(addr))


def rewrite(s, frame_ebp):
    """The accesses in s, split as above."""
    out = []
    i = 0
    while True:
        m = _ACCESS.search(s, i)
        if not m:
            out.append(s[i:])
            return "".join(out)
        out.append(s[i:m.start()])
        kind, width = m.group(1), m.group(2)
        open_i = m.end() - 1
        close_i = _match_paren(s, open_i)
        if close_i < 0:
            out.append(s[m.start():])
            return "".join(out)
        addr_raw = s[open_i + 1:close_i]
        addr = rewrite(addr_raw, frame_ebp)
        stack = _is_stack(addr_raw, frame_ebp)
        k = close_i + 1
        while k < len(s) and s[k] in " \t":
            k += 1
        if k < len(s) and s[k] == "=" and (k + 1 >= len(s) or s[k + 1] != "="):
            end = _statement_end(s, k + 1)
            if end >= 0:
                value = rewrite(s[k + 1:end], frame_ebp).strip()
                out.append(f"ST{width}{'_STK' if stack else ''}({addr}, {value})")
                i = end            # the ';' itself is kept
                continue
        out.append(f"{kind}{width}{'_STK' if stack else ''}({addr})")
        i = close_i + 1


def mem_order_function(code, frame_ebp):
    """--ordered-mem over one lifted function: its body only (the head and
    the closing brace carry no accesses)."""
    head = code.find("(void)\n{\n")
    if head < 0:
        return code
    body_start = head + len("(void)\n{\n")
    end = code.rstrip().rfind("}")
    if end <= body_start:
        return code
    return code[:body_start] + rewrite(code[body_start:end], frame_ebp) + code[end:]
