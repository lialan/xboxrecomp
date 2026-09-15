import json

from tools.doctor import (_flatten_recomp_stats, build_report, parse_icall_feedback,
                          parse_runtime_log, rank_priorities)


def test_flatten_recomp_stats_merges_categories():
    stats = {
        "game": {"total": 10, "translated": 9, "failed": 1,
                 "unimplemented": {"foo": [1, 2], "bar": [3]}},
        "crt": {"total": 5, "translated": 5, "failed": 0,
                "unimplemented": {"foo": [4]}},
    }
    out = _flatten_recomp_stats(stats)
    assert out["total"] == 15
    assert out["translated"] == 14
    assert out["failed"] == 1
    assert out["unimplemented"] == {"foo": 3, "bar": 1}


def test_parse_icall_feedback_tolerates_truncated_and_merges_flags(tmp_path):
    path = tmp_path / "icalls.txt"
    path.write_text("# icall-feedback v1\n0x1000 1\n0x2000 2\n0x1000 2\ntruncated\n")
    out = parse_icall_feedback(path)
    assert out["targets"] == 2
    assert out["resolved"] == 1
    assert out["unresolved"] == 2
    assert out["both"] == 1
    assert out["unresolved_vas"] == [0x1000, 0x2000]


def test_parse_runtime_log_counts_known_problem_classes(tmp_path):
    path = tmp_path / "run.log"
    path.write_text(
        "[KERNEL] unimplemented ordinal 123\n"
        "[D3D8] unsupported render state 999\n"
        "audio unsupported stream packet\n"
        "unresolved icall target 0x12345678\n"
    )
    out = parse_runtime_log(path)
    assert sum(out["kernel_stub"].values()) == 1
    assert sum(out["d3d_unsupported"].values()) == 1
    assert sum(out["audio_unsupported"].values()) == 1
    assert sum(out["unresolved_icall"].values()) == 1


def test_build_report_ranks_translation_and_icall_failures(tmp_path):
    functions = tmp_path / "functions.json"
    identified = tmp_path / "identified.json"
    abi = tmp_path / "abi.json"
    recomp = tmp_path / "recomp.json"
    icalls = tmp_path / "icalls.txt"
    functions.write_text(json.dumps([{"start": "0x1000"}, {"start": "0x2000"}]))
    identified.write_text(json.dumps({"0x1000": {}}))
    abi.write_text(json.dumps({"0x1000": {}, "0x2000": {}}))
    recomp.write_text(json.dumps({"total": 2, "translated": 1, "failed": 1,
                                  "unimplemented": {"fxam": [0x1010]}}))
    icalls.write_text("0x3000 2\n")
    report = build_report(str(functions), str(identified), str(abi), str(recomp),
                          str(icalls), None)
    reasons = " ".join(item["reason"] for item in report["priorities"])
    assert report["pipeline"]["functions"] == 2
    assert "failed translation" in reasons
    assert "unimplemented instructions" in reasons
    assert "indirect targets were unresolved" in reasons


def test_rank_priorities_has_clean_fallback():
    report = {
        "pipeline": {"missing_artifacts": []},
        "recompiler": {"failed": 0, "unimplemented": {}},
        "icalls": {"unresolved": 0},
        "runtime": {},
    }
    out = rank_priorities(report)
    assert out == [{"severity": "info", "area": "bring-up",
                    "reason": "no known blockers found in supplied artifacts/logs"}]
