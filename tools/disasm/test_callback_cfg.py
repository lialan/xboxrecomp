"""Weak callbacks need a closed CFG, not a short linear path to a ret."""
import struct

import pytest

from tools.disasm.test_returning_body_switch import BASE, _engine, _dispatching_body
from tools.disasm.functions import FunctionDetector
from tools.disasm.labels import LabelManager
from tools.disasm.loader import BinaryImage, SectionInfo


def test_long_callback_and_loop_have_no_instruction_budget():
    for body in (b"\x40" * 320 + b"\xc3", b"\x40\xeb\xfd"):
        engine, _ = _engine(body)
        assert engine.probes_as_callback_body(BASE, BASE + len(body))


@pytest.mark.parametrize("body", [
    b"\x74\x02\xc3\x90\x0f",  # ret on one arm, invalid other arm
    b"\x74\x02\xc3",           # branch outside the gap
    b"\x74\xff\xc3",           # branch into its own instruction
    b"\x74\x01\xb8\x00\x00\x00\x00\xc3",  # overlapping streams
    b"\x40",                    # falls off the gap
    b"\xff\xe0",                # unresolved indirect jump
    b"\x74\x01\xc3\xf4",       # ret does not excuse privileged other arm
    b"\xcd\x03\xc3", b"\x0f\x0b\xc3", b"\xfa\xc3",
    b"\x0f\x20\xc0\xc3", b"\xec\xc3", b"\xcc\xc3",
])
def test_invalid_or_open_cfg_is_rejected(body):
    engine, _ = _engine(body)
    assert not engine.probes_as_callback_body(BASE, BASE + len(body))


def test_no_return_call_padding_is_the_only_trap_exception():
    body = b"\xe8\x00\x01\x00\x00\xcc"
    engine, _ = _engine(body)
    assert engine.probes_as_callback_body(BASE, BASE + len(body))
    body = b"\x74\x05" + body
    engine, _ = _engine(body)
    assert not engine.probes_as_callback_body(BASE, BASE + len(body))


def test_all_measured_switch_arms_must_close_inside_gap():
    body, table = _dispatching_body(BASE)
    engine, _ = _engine(body)
    assert engine.probes_as_callback_body(BASE, BASE + len(body))
    engine.image.data = body[:10] + b"\xfa" + body[11:]
    assert not engine.probes_as_callback_body(BASE, BASE + len(body))
    engine.image.data = body[:24] + struct.pack("<III", BASE + 7, BASE + 10, BASE - 1)
    assert not engine.probes_as_callback_body(BASE, BASE + len(body))


def test_next_function_is_a_hard_bound():
    engine, _ = _engine(b"\x40\xc3")
    assert not engine.probes_as_callback_body(BASE, BASE + 1)


def test_table_word_inside_a_recovered_callback_is_not_a_new_entry():
    body = b"\xc3" + b"\xcc" * 15 + b"\x53" + b"\x40" * 320 + b"\x5b\xc3"
    table = struct.pack("<II", BASE + 16, BASE + 17)
    text = SectionInfo('.text', BASE, len(body), 0, len(body), False, True, '')
    data = SectionInfo('.data', BASE + 0x1000, len(table), len(body), len(table), False, False, '')
    image = BinaryImage('synthetic', body + table, 0, 0x20000, BASE, 0, [text, data])
    from tools.disasm.engine import DisasmEngine
    engine = DisasmEngine(image)
    engine.linear_sweep(text)
    detector = FunctionDetector(engine, image, None, LabelManager())
    detector._pass_known_addresses()
    detector._build_functions([text])
    assert detector._pass_data_ptr_targets([text])
    assert BASE + 16 in detector._alias_entries
    assert BASE + 17 not in detector._alias_entries
