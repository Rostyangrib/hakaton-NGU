"""Run the unmodified official interactor and measure the solution process on Windows or Linux."""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time


ROOT = Path(__file__).resolve().parents[1]


class ProcessMemoryCounters(ctypes.Structure):
    _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD)] + [
        (name, ctypes.c_size_t) for name in (
            "PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage",
            "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage", "QuotaNonPagedPoolUsage",
            "PagefileUsage", "PeakPagefileUsage",
        )
    ]


def windows_api():
    if sys.platform != "win32":
        raise RuntimeError("This measurement script uses Windows process accounting")
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    memory_api = ctypes.WinDLL("psapi", use_last_error=True)
    kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    kernel.GetProcessTimes.restype = wintypes.BOOL
    memory_api.GetProcessMemoryInfo.argtypes = [
        wintypes.HANDLE, ctypes.POINTER(ProcessMemoryCounters), wintypes.DWORD,
    ]
    memory_api.GetProcessMemoryInfo.restype = wintypes.BOOL
    return kernel, memory_api


def peak_working_set(memory_api, process):
    counters = ProcessMemoryCounters()
    counters.cb = ctypes.sizeof(counters)
    if memory_api.GetProcessMemoryInfo(int(process._handle), ctypes.byref(counters), counters.cb):
        return counters.PeakWorkingSetSize
    return 0


def process_cpu_times(kernel, process):
    creation, exit_time, system, user = (wintypes.FILETIME() for _ in range(4))
    if not kernel.GetProcessTimes(int(process._handle), *(ctypes.byref(x) for x in (creation, exit_time, system, user))):
        raise ctypes.WinError(ctypes.get_last_error())
    def seconds(value):
        return ((value.dwHighDateTime << 32) + value.dwLowDateTime) / 10_000_000
    return seconds(user), seconds(system)


def scenario(path):
    tokens = path.read_text(encoding="utf-8-sig").split()
    weight, n = float(tokens[0]), int(tokens[1])
    instructions = list(map(int, tokens[2:]))
    if len(instructions) != n + 1 or instructions[-1] != 0:
        raise ValueError(f"Invalid test format: {path}")
    return weight, instructions[:-1]


def replay_transcript(instructions, transcript):
    """Independently recover final trench length and remaining pipe meters."""
    answers = iter(map(int, transcript.read_text().split()))
    trench = 0
    pipes = [None]
    occupied = 0
    for command in instructions:
        if command > 0:
            digs, position = next(answers), next(answers)
            trench += digs * 20
            pipes.append((position, command))
            occupied += command
        else:
            position = next(answers)
            expected, length = pipes[-command]
            if expected != position:
                raise ValueError("Removal position mismatch in official transcript")
            occupied -= length
            pipes[-command] = None
    if next(answers, None) is not None:
        raise ValueError("Unexpected extra output in official transcript")
    if trench <= 0:
        raise ValueError("Nonpositive trench length")
    return occupied, trench


def check_protocol(solution, env):
    """Require a reply before sending the next command, then cross buffer boundaries."""
    process = subprocess.Popen([str(solution)], env=env, stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    replies = queue.Queue()

    def read_replies():
        for line in iter(process.stdout.readline, b""):
            replies.put(line)

    reader = threading.Thread(target=read_replies, daemon=True)
    reader.start()
    cases = [
        ([b" \t1\r\n"], b"1 0"),
        ([b"4\n"], b"0 1"),
        ([b"1", b"5", b"\n"], b"0 5"),
        ([b"-", b"2", b"\n"], b"1"),
        ([b"3 \n"], b"0 1"),
        ([b"-1\n"], b"0"),
    ]
    try:
        for fragments, expected in cases:
            for fragment in fragments:
                process.stdin.write(fragment)
                process.stdin.flush()
            actual = replies.get(timeout=3).rstrip(b"\r\n")
            if actual != expected:
                raise ValueError(f"Streaming reply: expected {expected!r}, got {actual!r}")
        process.stdin.write(b"0\n999\n")
        process.stdin.flush()
        process.stdin.close()
        process.wait(timeout=3)
        reader.join(timeout=3)
        if process.returncode or reader.is_alive() or not replies.empty() or process.stderr.read():
            raise ValueError("Streaming termination failed")
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        reader.join(timeout=3)
        for stream in (process.stdin, process.stdout, process.stderr):
            stream.close()

    commands = [1] * 5000 + [-pid for pid in range(1, 5001)] + [0]
    bulk = subprocess.run([str(solution)], input=("\n".join(map(str, commands)) + "\n").encode(),
                          env=env, capture_output=True, timeout=10, check=True)
    expected = [value for i in range(5000) for value in (int(i % 20 == 0), i)] + list(range(5000))
    if bulk.stderr or list(map(int, bulk.stdout.split())) != expected:
        raise ValueError("Buffered input boundary check failed")
    for payload, expected_output, valid in (
        (b"1", b"1 0", True), (b"0\n41\n", b"", True),
        (b"41\n", b"", False), (b"-1\n", b"", False),
    ):
        result = subprocess.run([str(solution)], input=payload, env=env,
                                capture_output=True, timeout=3)
        if result.stdout.strip() != expected_output or (result.returncode == 0) != valid:
            raise ValueError(f"Protocol edge case failed: {payload!r}")
        if bool(result.stderr.strip()) == valid:
            raise ValueError(f"Unexpected diagnostic status: {payload!r}")
    print("Protocol: immediate replies, fragmented input, CRLF, EOF, zero, errors, 10000-command buffer check passed", flush=True)
    return {"passed": True, "streamed_replies": len(cases), "bulk_commands": 10000,
            "edge_cases": 4, "reply_timeout_seconds": 3}


def run_case(test_path, solution, interactor, result_dir, env, kernel, memory_api):
    weight, commands = scenario(test_path)
    folder = result_dir / f"test-{int(test_path.stem):02d}"
    folder.mkdir(parents=True, exist_ok=False)
    (folder / "answer.txt").write_text("")
    started = time.monotonic()
    peak_bytes = 0
    with (folder / "interactor.stderr.txt").open("wb") as interactor_error, (folder / "solution.stderr.txt").open("wb") as solution_error:
        checker = subprocess.Popen(
            [str(interactor), str(test_path), "transcript.txt", "answer.txt", "report.txt"],
            cwd=folder, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=interactor_error,
        )
        solver = None
        try:
            solver = subprocess.Popen(
                [str(solution)], cwd=folder, env=env, stdin=checker.stdout,
                stdout=checker.stdin, stderr=solution_error,
            )
            checker.stdout.close()
            checker.stdin.close()
            deadline = time.monotonic() + 60
            while True:
                if sys.platform == "win32":
                    peak_bytes = max(peak_bytes, peak_working_set(memory_api, solver))
                    if solver.poll() is not None:
                        peak_bytes = max(peak_bytes, peak_working_set(memory_api, solver))
                        user_cpu, kernel_cpu = process_cpu_times(kernel, solver)
                        break
                else:
                    pid, status, usage = os.wait4(solver.pid, os.WNOHANG)
                    if pid:
                        solver.returncode = os.waitstatus_to_exitcode(status)
                        user_cpu, kernel_cpu = usage.ru_utime, usage.ru_stime
                        peak_bytes = usage.ru_maxrss * 1024
                        break
                if time.monotonic() > deadline:
                    raise TimeoutError(f"Test {test_path.stem}: 60 s wall timeout")
                time.sleep(.005)
            cpu_seconds = user_cpu + kernel_cpu
            checker.wait(timeout=10)
        finally:
            for process in (solver, checker):
                if process is not None and process.poll() is None:
                    process.kill()
                    process.wait()
    wall_seconds = time.monotonic() - started
    if solver.returncode != 0 or checker.returncode != 0:
        raise RuntimeError(f"Test {test_path.stem}: solver={solver.returncode}, interactor={checker.returncode}; see {folder}")
    stderr = (folder / "solution.stderr.txt").read_text(errors="replace")
    if stderr.strip():
        raise RuntimeError(f"Test {test_path.stem}: unexpected solution stderr: {stderr[:1000]}")
    score = float((folder / "score.txt").read_text().strip())
    if not math.isfinite(score):
        raise ValueError("Nonfinite score")
    occupied, trench = replay_transcript(commands, folder / "transcript.txt")
    kpi = occupied / trench
    if not math.isclose(score, kpi * weight, rel_tol=1e-8, abs_tol=1e-8):
        raise ValueError("Official score does not match independent transcript replay")
    if not peak_bytes:
        raise RuntimeError("Peak working set was not measured")
    return {
        "test": int(test_path.stem), "commands": len(commands), "weight": weight,
        "remaining_pipe_m": occupied, "trench_m": trench, "kpi": kpi,
        "weighted_score": kpi * weight, "cpu_seconds": cpu_seconds,
        "user_cpu_seconds": user_cpu, "kernel_cpu_seconds": kernel_cpu,
        "peak_memory_mib": peak_bytes / 2**20,
        "memory_metric": "peak_working_set" if sys.platform == "win32" else "peak_rss",
        "wall_seconds": wall_seconds,
        "within_cpu_limit": cpu_seconds < 5.0,
        "within_memory_limit": peak_bytes < 256 * 2**20,
        "test_sha256": hashlib.sha256(test_path.read_bytes()).hexdigest(),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    windows = sys.platform == "win32"
    if not windows and not sys.platform.startswith("linux"):
        raise RuntimeError("Supported process accounting platforms: Windows and Linux")
    parser.add_argument("--language", choices=("cpp", "cangjie"), default="cpp")
    parser.add_argument("--solution", type=Path, default=None)
    parser.add_argument("--source", type=Path, default=None,
                        help="Source file for the selected binary, recorded in the report")
    parser.add_argument("--compiler", type=Path, default=None)
    parser.add_argument("--sdk", type=Path, default=None)
    parser.add_argument("--test", type=int, choices=range(1, 16), default=None)
    parser.add_argument("--report", type=Path, default=None)
    parser.add_argument("--reference-traces", type=Path, default=None,
                        help="Compare numeric replies with an earlier full run's test-XX/transcript.txt")
    args = parser.parse_args()
    if args.language == "cpp":
        default_solution = ROOT / ("build/solution-cpp.exe" if windows else "build/solution-cpp")
        source = ROOT / "solution.cpp"
    else:
        default_solution = ROOT / ("build/solution.exe" if windows else "build/solution-linux")
        source = ROOT / "solution.cj"
    source = (args.source or source).resolve(strict=True)
    solution = (args.solution or default_solution).resolve(strict=True)
    env = os.environ.copy()
    if args.language == "cpp":
        local_compiler = ROOT / ".tools/w64devkit/bin/g++.exe"
        compiler = str(args.compiler or (local_compiler if windows and local_compiler.exists() else "g++"))
    else:
        default_sdk = ROOT / (".tools/cangjie-1.0.5/cangjie" if windows else ".tools/cangjie-linux-1.0.5/cangjie")
        sdk = (args.sdk or Path(os.environ.get("CANGJIE_HOME", default_sdk))).resolve(strict=True)
        env["CANGJIE_HOME"] = str(sdk)
        runtime_paths = [str(sdk / path) for path in (
            "lib/windows_x86_64_cjnative", "runtime/lib/windows_x86_64_cjnative",
        )] if windows else [str(sdk / path) for path in (
            "lib/linux_x86_64_cjnative", "runtime/lib/linux_x86_64_cjnative",
        )]
        env["PATH"] = os.pathsep.join([str(sdk / "bin"), str(sdk / "tools/bin"),
                                       str(sdk / "tools/lib")] + (runtime_paths if windows else [])) + os.pathsep + env["PATH"]
        if not windows:
            env["LD_LIBRARY_PATH"] = os.pathsep.join(runtime_paths) + os.pathsep + env.get("LD_LIBRARY_PATH", "")
        compiler = str(sdk / ("bin/cjc.exe" if windows else "bin/cjc"))
    version = subprocess.check_output([compiler, "--version"], env=env, text=True).strip()
    kernel, memory_api = windows_api() if windows else (None, None)
    protocol = check_protocol(solution, env) if args.test is None else None
    materials = ROOT / "case2_materials"
    tests = sorted((materials / "tests").glob("*.in"), key=lambda p: int(p.stem))
    if [int(p.stem) for p in tests] != list(range(1, 16)):
        raise ValueError("Expected the 15 provided open tests, numbered 1..15")
    if args.test is not None:
        tests = [p for p in tests if int(p.stem) == args.test]
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    result_dir = ROOT / "build/open-tests" / stamp
    result_dir.mkdir(parents=True)
    rows = []
    # The official interactor flushes its transcript after every command. Write
    # these logs on the native temporary filesystem, not a watched checkout or
    # WSL's /mnt/c bridge, and copy them to the workspace after measurement.
    with tempfile.TemporaryDirectory(prefix="case2-open-") as native_tmp:
        execution_dir = Path(native_tmp)
        try:
            for test_path in tests:
                row = run_case(test_path, solution, materials / ("interactor.exe" if windows else "interactor"),
                               execution_dir, env, kernel, memory_api)
                if args.reference_traces is not None:
                    relative = Path(f"test-{row['test']:02d}/transcript.txt")
                    reference = (args.reference_traces / relative).read_text().split()
                    if reference != (execution_dir / relative).read_text().split():
                        raise ValueError(f"Test {row['test']}: numeric replies differ from the reference run")
                    row["matches_reference"] = True
                rows.append(row)
                print(f"Test {row['test']:2d}: KPI={row['kpi']:.6f}, score={row['weighted_score']:.6f}, "
                      f"CPU={row['cpu_seconds']:.3f}s, peak={row['peak_memory_mib']:.1f} MiB", flush=True)
        finally:
            shutil.copytree(execution_dir, result_dir, dirs_exist_ok=True)
    report = {
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "platform": sys.platform, "language": args.language, "compiler": version,
        "source_file": source.name,
        "artifacts_directory": result_dir.relative_to(ROOT).as_posix(),
        "transcript_storage": "native system temporary directory; copied to artifacts_directory after measurement",
        "solution_sha256": hashlib.sha256(solution.read_bytes()).hexdigest(),
        "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "interactor_sha256": hashlib.sha256((materials / ("interactor.exe" if windows else "interactor")).read_bytes()).hexdigest(),
        "measurement": ("Windows GetProcessTimes: user+kernel CPU; GetProcessMemoryInfo: peak working set; solution process only"
                        if windows else "Linux wait4 rusage: user+system CPU, ru_maxrss peak RSS; solution process only"),
        "runtime_overrides": {key: value for key, value in env.items()
                              if key.lower().startswith("cj")},
        "protocol": protocol,
        "all_match_reference": True if args.reference_traces is not None else None,
        "test_count": len(rows),
        "tests": rows,
        "total_weighted_score": sum(row["weighted_score"] for row in rows),
        "sum_test_weights": sum(row["weight"] for row in rows),
        "all_valid": True,
        "all_within_limits": all(row["within_cpu_limit"] and row["within_memory_limit"] for row in rows),
    }
    report_path = args.report or (ROOT / f"reports/open_tests_{args.language}_{'windows' if windows else 'linux'}.json" if args.test is None
                                 else ROOT / f"build/test-{args.test}-probe.json")
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print("Weighted score:", report["total_weighted_score"], "of sum of weights", report["sum_test_weights"])
    print("Report:", report_path)
    return 0 if report["all_within_limits"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
