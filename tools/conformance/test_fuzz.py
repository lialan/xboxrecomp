import pytest

from tools.conformance.fuzz import generate_cases


def test_fuzz_is_deterministic():
    a = generate_cases(32, 0x1234)
    b = generate_cases(32, 0x1234)
    assert a == b


def test_fuzz_seed_changes_corpus():
    assert generate_cases(16, 1) != generate_cases(16, 2)


def test_fuzz_cases_are_runner_compatible():
    for case in generate_cases(100, 0xC0FFEE):
        assert set(("name", "why", "asm", "inputs", "kind", "tol")) <= set(case)
        assert case["kind"] == "gpr"
        assert case["asm"]
        assert case["inputs"]
        assert all(len(pair) == 2 for pair in case["inputs"])
        assert all(0 <= value <= 0xFFFFFFFF for pair in case["inputs"] for value in pair)


def test_fuzz_count_must_be_positive():
    with pytest.raises(ValueError):
        generate_cases(0, 1)
