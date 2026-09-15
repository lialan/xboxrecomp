"""Bring-up health report for an xboxrecomp title.

The doctor combines static pipeline artifacts, recompilation statistics, indirect
call feedback and an optional runtime log into one compact report.  It is meant
to answer the first bring-up question: "what is still unsupported or quietly
wrong enough to investigate next?"

Usage:

    py -3 tools/doctor.py
    py -3 tools/doctor.py --runtime-log game.log --icall-feedback icall-feedback.txt
    py -3 tools/doctor.py --json doctor.json
"""

from __future__ import annotations

import argparse
import json
import os
import re
from collections import Counter


DEFAULTS = {
    "functions": os.path.join("tools", "disasm", "output", "functions.json"),
    "identified": os.path.join("tools", "func_id", "output", "identified_functions.json"),
    "abi": os.path.join("tools", "abi_analysis", "output", "abi_functions.json"),
    "recomp": os.path.join("tools", "recomp", "output", "summary.json"),
}


def _load_json(path):
    if not path or not os.path.exists(path):
        return None
    try:
        with open(path, "r", encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return None


def _count_entries(value):
    if isinstance(value, (list, dict)):
        return len(value)
    return 0


def _flatten_recomp_stats(stats):
    """Normalize single-batch and per-category recompiler summary shapes."""
    out = {"total": 0, "translated": 0, "failed": 0, "unimplemented": Counter()}
    if not isinstance(stats, dict):
        return out
    batches = [stats] if "total" in stats else [v for v in stats.values() if isinstance(v, dict)]
    for batch in batches:
        out["total"] += int(batch.get("total", 0) or 0)
        out["translated"] += int(batch.get("translated", 0) or 0)
        out["failed"] += int(batch.get("failed", 0) or 0)
        for mnemonic, addrs in (batch.get("unimplemented") or {}).items():
            if isinstance(addrs, list):
                out["unimplemented"][mnemonic] += len(addrs)
            else:
                try:
                    out["unimplemented"][mnemonic] += int(addrs)
                except (TypeError, ValueError):
                    pass
    out["unimplemented"] = dict(out["unimplemented"].most_common())
    return out


def parse_icall_feedback(path):
    """Parse the crash-tolerant `VA flags` feedback file written by the runtime."""
    result = {"targets": 0, "resolved": 0, "unresolved": 0, "both": 0, "unresolved_vas": []}
    if not path or not os.path.exists(path):
        return result
    seen = {}
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                parts = line.split()
                if len(parts) < 2:
                    continue
                try:
                    va = int(parts[0], 0)
                    flags = int(parts[1], 0)
                except ValueError:
                    continue
                seen[va] = seen.get(va, 0) | flags
    except OSError:
        return result
    result["targets"] = len(seen)
    for va, flags in sorted(seen.items()):
        if flags & 1:
            result["resolved"] += 1
        if flags & 2:
            result["unresolved"] += 1
            result["unresolved_vas"].append(va)
        if flags == 3:
            result["both"] += 1
    return result


_LOG_PATTERNS = (
    ("unresolved_icall", re.compile(r"(?:unresolved|failed).*?(?:icall|indirect call).*?(0x[0-9a-fA-F]+)", re.I)),
    ("kernel_stub", re.compile(r"(?:kernel|xboxkrnl).*?(?:stub|unimplemented|unsupported).*?([A-Za-z_][A-Za-z0-9_]*|ordinal\s+\d+)", re.I)),
    ("d3d_unsupported", re.compile(r"(?:D3D8|D3D|NV2A).*?(?:unsupported|unimplemented|unknown).*?([^\r\n]+)", re.I)),
    ("audio_unsupported", re.compile(r"(?:DSOUND|DirectSound|APU|audio|WMA).*?(?:unsupported|unimplemented|stub).*?([^\r\n]+)", re.I)),
    ("unhandled_instruction", re.compile(r"(?:unhandled|unimplemented).*?(?:instruction|mnemonic).*?\b([A-Za-z][A-Za-z0-9]+)\b", re.I)),
)


def parse_runtime_log(path):
    counters = {name: Counter() for name, _ in _LOG_PATTERNS}
    if not path or not os.path.exists(path):
        return {name: {} for name, _ in _LOG_PATTERNS}
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                for name, pattern in _LOG_PATTERNS:
                    match = pattern.search(line)
                    if match:
                        value = " ".join(match.group(1).strip().split())[:160]
                        counters[name][value] += 1
    except OSError:
        pass
    return {name: dict(counter.most_common(25)) for name, counter in counters.items()}


def build_report(functions=None, identified=None, abi=None, recomp=None,
                 icall_feedback=None, runtime_log=None):
    fns = _load_json(functions)
    ids = _load_json(identified)
    abi_data = _load_json(abi)
    recomp_data = _load_json(recomp)
    report = {
        "pipeline": {
            "functions": _count_entries(fns),
            "identified_functions": _count_entries(ids),
            "abi_functions": _count_entries(abi_data),
            "missing_artifacts": [
                name for name, path in (("functions", functions), ("identified", identified),
                                        ("abi", abi), ("recomp", recomp))
                if not path or not os.path.exists(path)
            ],
        },
        "recompiler": _flatten_recomp_stats(recomp_data),
        "icalls": parse_icall_feedback(icall_feedback),
        "runtime": parse_runtime_log(runtime_log),
    }
    report["priorities"] = rank_priorities(report)
    return report


def rank_priorities(report):
    priorities = []
    recomp = report["recompiler"]
    icalls = report["icalls"]
    runtime = report["runtime"]
    if recomp["failed"]:
        priorities.append({"severity": "high", "area": "recompiler",
                           "reason": f"{recomp['failed']} recovered functions failed translation"})
    unimpl_total = sum(recomp["unimplemented"].values())
    if unimpl_total:
        top = next(iter(recomp["unimplemented"]), "unknown")
        priorities.append({"severity": "high", "area": "cpu",
                           "reason": f"{unimpl_total} unimplemented instructions remain; top mnemonic: {top}"})
    if icalls["unresolved"]:
        priorities.append({"severity": "high", "area": "control-flow",
                           "reason": f"{icalls['unresolved']} observed indirect targets were unresolved"})
    for key, area in (("kernel_stub", "kernel"), ("d3d_unsupported", "graphics"),
                      ("audio_unsupported", "audio"), ("unhandled_instruction", "cpu")):
        count = sum(runtime.get(key, {}).values())
        if count:
            priorities.append({"severity": "medium", "area": area,
                               "reason": f"{count} runtime warning hit(s) matched {key}"})
    if report["pipeline"]["missing_artifacts"]:
        priorities.append({"severity": "info", "area": "pipeline",
                           "reason": "missing artifacts: " + ", ".join(report["pipeline"]["missing_artifacts"])})
    if not priorities:
        priorities.append({"severity": "info", "area": "bring-up",
                           "reason": "no known blockers found in supplied artifacts/logs"})
    return priorities


def _print_report(report):
    p = report["pipeline"]
    r = report["recompiler"]
    i = report["icalls"]
    print("xboxrecomp doctor")
    print("=================")
    print(f"Recovered functions : {p['functions']}")
    print(f"Identified functions: {p['identified_functions']}")
    print(f"ABI entries          : {p['abi_functions']}")
    print(f"Translated functions : {r['translated']}/{r['total']} ({r['failed']} failed)")
    print(f"Unimplemented insns  : {sum(r['unimplemented'].values())}")
    print(f"ICALL targets        : {i['targets']} ({i['unresolved']} unresolved)")
    if r["unimplemented"]:
        print("\nTop unimplemented instructions:")
        for mnemonic, count in list(r["unimplemented"].items())[:10]:
            print(f"  {count:6d}  {mnemonic}")
    print("\nNext things to investigate:")
    for item in report["priorities"]:
        print(f"  [{item['severity'].upper():6}] {item['area']}: {item['reason']}")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--functions", default=DEFAULTS["functions"])
    ap.add_argument("--identified", default=DEFAULTS["identified"])
    ap.add_argument("--abi", default=DEFAULTS["abi"])
    ap.add_argument("--recomp", default=DEFAULTS["recomp"])
    ap.add_argument("--icall-feedback")
    ap.add_argument("--runtime-log")
    ap.add_argument("--json", metavar="PATH", help="also write the full report as JSON")
    args = ap.parse_args(argv)
    report = build_report(args.functions, args.identified, args.abi, args.recomp,
                          args.icall_feedback, args.runtime_log)
    _print_report(report)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(report, fh, indent=2)
            fh.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
