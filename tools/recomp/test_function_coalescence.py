"""Explicit boundary repair, using only independently constructed x86 code."""
import copy
import json

import pytest

from . import config
from .translator import BatchTranslator, FunctionTranslator, load_coalescences


BASE = 0x10000
# Select a cap, then return min(eax, cap). Both CMPs feed the shared JGE.
# test ecx,ecx; jz short; mov edi,12; cmp eax,edi; jmp join;
# short: mov edi,6; cmp eax,edi; join: jge done; mov edi,eax;
# done: mov eax,edi; ret
CLAMP = bytes.fromhex("85c97409bf0c00000039f8eb07bf0600000039f87d0289c789f8c3")
SPLITS = [BASE + 13, BASE + 20, BASE + 24]
END = BASE + len(CLAMP)


@pytest.fixture(autouse=True)
def layout(monkeypatch):
    monkeypatch.setattr(config, "_SECTIONS", [
        config.Section(".text", BASE, 0x400, 0, 0x400, True),
    ])


def function(start, end):
    return {"_addr": start, "start": hex(start), "end": end,
            "size": end - start, "section": ".text"}


def translator(body=CLAMP, splits=SPLITS):
    bounds = [BASE, *splits, BASE + len(body)]
    db = {start: function(start, end)
          for start, end in zip(bounds, bounds[1:])}
    return FunctionTranslator(body.ljust(0x400, b"\xcc"), db)


def test_repaired_clamp_matches_unsplit_translation():
    split = translator()
    old = split.translate_function(SPLITS[1], split.func_db[SPLITS[1]])
    assert "if (_flags /* jge" in old
    # Translation does not discover ownership, so it is safe to recover here.
    split.coalesce_function(BASE, END, SPLITS)
    repaired = split.translate_function(BASE, split.func_db[BASE])
    whole = translator(splits=[])
    reference = whole.translate_function(BASE, whole.func_db[BASE])
    assert repaired == reference
    assert "CMP_GE(" in repaired
    assert "if (_flags /* jge" not in repaired
    assert list(split.func_db) == [BASE]
    assert split.coalesced_function_starts == {BASE}
    assert split.func_db[BASE]["detection_method"] == "external_coalescence"


def test_removes_stale_fragment_ownership():
    subject = translator()
    subject._recovered_cfg[SPLITS[0]] = {"end": END}
    subject.owned_function_starts.add(SPLITS[0])
    subject.recovered_function_starts.add(SPLITS[0])
    subject.coalesce_function(BASE, END, SPLITS)
    assert SPLITS[0] not in subject._recovered_cfg
    assert SPLITS[0] not in subject.owned_function_starts
    assert SPLITS[0] not in subject.recovered_function_starts


@pytest.mark.parametrize("starts", [[], SPLITS[:-1], SPLITS[::-1],
                                   [SPLITS[0], *SPLITS], [BASE, *SPLITS]])
def test_requires_exact_sorted_interior_census(starts):
    subject = translator()
    before = copy.deepcopy(subject.func_db)
    with pytest.raises(ValueError):
        subject.coalesce_function(BASE, END, starts)
    assert subject.func_db == before
    assert not subject._recovered_cfg


@pytest.mark.parametrize("evidence", [
    {"has_prologue": True}, {"called_by": [hex(BASE + 0x100)]},
    {"external_entry": True},
    {"has_prologue": True, "seed_derived": True},
])
def test_independent_entry_evidence_is_never_discarded(evidence):
    subject = translator()
    subject.func_db[SPLITS[0]].update(evidence)
    before = copy.deepcopy(subject.func_db)
    with pytest.raises(ValueError, match="independent evidence"):
        subject.coalesce_function(BASE, END, SPLITS)
    assert subject.func_db == before


def test_rejects_call_to_interior_even_with_incomplete_metadata():
    # call inner; jmp inner; inner: ret
    subject = translator(bytes.fromhex("e802000000eb00c3"), [BASE + 7])
    with pytest.raises(ValueError, match="called from"):
        subject.coalesce_function(BASE, BASE + 8, [BASE + 7])


@pytest.mark.parametrize("padding", ["6690", "8d1b", "8da4240000000090"])
def test_only_proven_alignment_padding_closes_a_decode_gap(padding):
    pad = bytes.fromhex(padding)
    body = bytes([0xeb, len(pad)]) + pad + b"\xc3"
    interior = BASE + 2 + len(pad)
    subject = translator(body, [interior])
    subject.coalesce_function(BASE, BASE + len(body), [interior])
    assert subject._recovered_cfg[BASE]["end"] == BASE + len(body)


def test_unreached_live_instructions_are_not_alignment_padding():
    subject = translator(bytes.fromhex("eb0231c0c3"), [BASE + 4])
    with pytest.raises(ValueError, match="CFG gap"):
        subject.coalesce_function(BASE, BASE + 5, [BASE + 4])


def test_jump_table_can_follow_owned_code():
    end = BASE + 13
    body = (bytes.fromhex("31c0ff2485") + end.to_bytes(4, "little")
            + bytes.fromhex("40c34bc3")
            + (BASE + 9).to_bytes(4, "little")
            + (BASE + 11).to_bytes(4, "little"))
    subject = translator(body, [BASE + 9])
    subject.func_db[BASE + 9]["end"] = end
    subject.func_db[BASE + 0x100] = function(BASE + 0x100, BASE + 0x101)
    subject.coalesce_function(BASE, end, [BASE + 9])
    recovered = subject._recovered_cfg[BASE]
    assert recovered["end"] == end
    assert recovered["jump_tables"][end] == [BASE + 9, BASE + 11]


@pytest.mark.parametrize("mutation, message", [
    (lambda s: s.func_db[BASE].update(end=END + 1), "shrinks"),
    (lambda s: s.func_db[SPLITS[0]].update(end=END + 1), "crosses end"),
    (lambda s: s.func_db.update({BASE - 1: function(BASE - 1, BASE + 1)}),
     "preceding function overlaps"),
    (lambda s: setattr(s, "_ownership_ready", True), "before discovering"),
])
def test_rejects_conflicting_ownership(mutation, message):
    subject = translator()
    mutation(subject)
    before = copy.deepcopy(subject.func_db)
    with pytest.raises(ValueError, match=message):
        subject.coalesce_function(BASE, END, SPLITS)
    assert subject.func_db == before


def test_rejects_extent_outside_backed_code():
    with pytest.raises(ValueError, match="one code section"):
        translator().coalesce_function(BASE, BASE + 0x401, SPLITS)


def test_rejects_unproven_end():
    with pytest.raises(ValueError, match="not the requested end"):
        translator().coalesce_function(BASE, END + 1, SPLITS)


@pytest.mark.parametrize("entry", [
    {}, [None], [{"start": "0x10000", "end": "0x1001b"}],
    [{"start": 0x10000, "end": "0x1001b", "coalesce_starts": []}],
    [{"start": "0x100000000", "end": "0x1001b", "coalesce_starts": []}],
    [{"start": "0x10000", "end": "invalid", "coalesce_starts": []}],
    [{"start": "0x10000", "end": "0x1001b", "coalesce_starts": "0x1000d"}],
])
def test_invalid_recovery_json_fails_closed(tmp_path, entry):
    path = tmp_path / "bounds.json"
    path.write_text(json.dumps(entry), encoding="utf-8")
    with pytest.raises(ValueError):
        load_coalescences(path)


def test_batch_wires_repair_before_ownership_and_emission(tmp_path):
    subject = translator()
    entries = list(copy.deepcopy(subject.func_db).values())
    for entry in entries:
        entry["end"] = hex(entry["end"])
    image = tmp_path / "synthetic.xbe"
    functions = tmp_path / "functions.json"
    bounds = tmp_path / "bounds.json"
    image.write_bytes(subject.xbe_data)
    functions.write_text(json.dumps(entries), encoding="utf-8")
    bounds.write_text(json.dumps([{
        "start": hex(BASE), "end": hex(END),
        "coalesce_starts": [hex(start) for start in SPLITS],
    }]), encoding="utf-8")
    batch = BatchTranslator(image, functions, coalesce_json_paths=[bounds],
                            seh_prolog=0, seh_epilog=0)
    assert list(batch.func_db) == [BASE]
    assert "CMP_GE(" in batch.translate_single(BASE)
    assert batch.translate_single(SPLITS[0]) is None
