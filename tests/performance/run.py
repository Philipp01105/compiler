#!/usr/bin/env python3
"""Measure generated program runtime for -O0 and -O1, with correctness checks."""

import argparse
import datetime as dt
import hashlib
import json
import platform
import statistics
import subprocess
import sys
import time
from pathlib import Path


def normalized(data):
    return data.replace(b"\r\n", b"\n")


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def compile_case(compiler, source, output, level, timeout):
    command = [str(compiler), level, str(source), "-o", str(output)]
    result = subprocess.run(command, cwd=source.parent, capture_output=True,
                            timeout=timeout, check=False)
    if result.returncode or not output.is_file():
        raise RuntimeError(
            f"Compilation failed for {source.parent.name} {level}:\n"
            f"{result.stdout.decode(errors='replace')}"
            f"{result.stderr.decode(errors='replace')}")


def run_case(executable, expected, timeout):
    start = time.perf_counter_ns()
    result = subprocess.run([str(executable)], capture_output=True,
                            timeout=timeout, check=False)
    elapsed = time.perf_counter_ns() - start
    if result.returncode != 0 or normalized(result.stdout) != expected or result.stderr:
        raise RuntimeError(
            f"Incorrect result from {executable} (exit {result.returncode}):\n"
            f"stdout: {result.stdout!r}\nstderr: {result.stderr!r}\n"
            f"expected: {expected!r}")
    return elapsed


def measure_case(name, paths, expected, warmups, samples, timeout):
    for level in ("O0", "O1"):
        for _ in range(warmups):
            run_case(paths[level], expected, timeout)

    readings = {"O0": [], "O1": []}
    for index in range(samples):
        # Alternate execution order to reduce systematic thermal/drift bias.
        levels = ("O0", "O1") if index % 2 == 0 else ("O1", "O0")
        for level in levels:
            readings[level].append(run_case(paths[level], expected, timeout))

    medians = {level: statistics.median(values) for level, values in readings.items()}
    return {
        "name": name,
        "samples_ms": {level: [round(value / 1e6, 3) for value in values]
                       for level, values in readings.items()},
        "median_ms": {level: round(value / 1e6, 3) for level, value in medians.items()},
        "o1_over_o0": round(medians["O1"] / medians["O0"], 4),
        "speedup": round(medians["O0"] / medians["O1"], 4),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument("--samples", type=int, default=7)
    parser.add_argument("--timeout", type=float, default=30.0,
                        help="Timeout in seconds for one compilation or execution")
    parser.add_argument("--baseline", type=Path,
                        help="Prior JSON report from the same machine")
    parser.add_argument("--max-regression-percent", type=float,
                        help="Allowed O1 runtime increase against --baseline")
    args = parser.parse_args()
    if args.warmups < 0 or args.samples < 1 or args.timeout <= 0:
        parser.error("warmups must be nonnegative; samples and timeout must be positive")
    if args.max_regression_percent is not None and (
            not args.baseline or args.max_regression_percent < 0):
        parser.error("--max-regression-percent requires --baseline and a nonnegative value")

    compiler = args.compiler.resolve(strict=True)
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    baseline = None
    if args.baseline:
        previous = json.loads(args.baseline.read_text(encoding="utf-8"))
        baseline = {item["name"]: item for item in previous["cases"]}

    report = {
        "schema": 1,
        "timestamp_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "platform": platform.platform(),
        "processor": platform.processor(),
        "compiler": str(compiler),
        "compiler_sha256": sha256(compiler),
        "warmups": args.warmups,
        "samples": args.samples,
        "cases": [],
    }
    failed = []
    cases = sorted(Path(__file__).parent.glob("*/*.dmm"))
    if not cases:
        raise RuntimeError("No performance fixtures found")
    header = "case                 O0 median    O1 median    O1/O0    speedup"
    if baseline is not None:
        header += "   vs baseline"
    print(header)
    for source in cases:
        name = source.parent.name
        expected = normalized(source.with_suffix(".expected").read_bytes())
        paths = {}
        for level in ("O0", "O1"):
            executable = output_dir / f"{name}_{level}{'.exe' if sys.platform == 'win32' else ''}"
            compile_case(compiler, source.resolve(), executable, f"-{level}", args.timeout)
            paths[level] = executable
        result = measure_case(name, paths, expected, args.warmups, args.samples,
                              args.timeout)
        result["source_sha256"] = sha256(source)
        if baseline is not None:
            if name not in baseline:
                raise RuntimeError(f"Baseline is missing case {name}")
            if baseline[name].get("source_sha256") != result["source_sha256"]:
                raise RuntimeError(f"Baseline uses a different source for {name}")
            old_ms = baseline[name]["median_ms"]["O1"]
            result["regression_percent"] = round(
                100 * (result["median_ms"]["O1"] / old_ms - 1), 2)
            if (args.max_regression_percent is not None and
                    result["regression_percent"] > args.max_regression_percent):
                failed.append(f"{name}: O1 increased {result['regression_percent']}%")
        report["cases"].append(result)
        line = (f"{name:20} {result['median_ms']['O0']:9.3f} ms "
                f"{result['median_ms']['O1']:9.3f} ms "
                f"{result['o1_over_o0']:8.3f} {result['speedup']:8.3f}x")
        if baseline is not None:
            line += f" {result['regression_percent']:+12.2f}%"
        print(line)

    report_path = output_dir / "report.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Report: {report_path}")
    if failed:
        print("Performance regression threshold exceeded:", file=sys.stderr)
        for failure in failed:
            print(f"  {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.TimeoutExpired, ValueError, KeyError) as error:
        print(f"Performance suite failed: {error}", file=sys.stderr)
        sys.exit(1)
