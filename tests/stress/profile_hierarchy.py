#!/usr/bin/env python3
# Copyright 2026 The Phantasos Authors
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Times and profiles `phantasos-stage0 check` on generated class hierarchies.

For each shape and size, generates a file with gen_hierarchy.py, runs `check`
on it, and prints wall time and peak memory. With --perf, also records a
`perf` profile and prints the functions with the most self and inclusive
time. Build stage0 with -DCMAKE_BUILD_TYPE=RelWithDebInfo and
-fno-omit-frame-pointer for readable call stacks.

Usage: python tests/stress/profile_hierarchy.py --stage0 <exe> [--shapes chain,wide]
           [--classes 1000,10000,200000] [--perf] [--top 15] [--out DIR]
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
# Frames above the compiler's own code; every sample includes them.
STARTUP = {"_start", "main", "__libc_start_main@@GLIBC_2.34", "__libc_start_call_main", "::run_frontend"}


def run(cmd: list[str], timeout: float) -> tuple[float, int, int | None]:
    """Runs cmd and returns wall seconds, its peak RSS in KiB, and its exit code (None on timeout)."""
    start = time.perf_counter()
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if not hasattr(os, "wait4"):  # Windows: no per-process peak memory
        try:
            return time.perf_counter() - start, 0, proc.wait(timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            return time.perf_counter() - start, 0, None
    deadline = start + timeout
    while True:
        # wait4 reports this child's own peak memory; RUSAGE_CHILDREN would mix runs.
        pid, status, usage = os.wait4(proc.pid, os.WNOHANG)
        if pid:
            proc.returncode = os.waitstatus_to_exitcode(status)
            return time.perf_counter() - start, usage.ru_maxrss, proc.returncode
        if time.perf_counter() > deadline:
            proc.kill()
            proc.wait()
            return time.perf_counter() - start, 0, None
        time.sleep(0.05)


def short(symbol: str) -> str:
    # Drop parameter lists and template arguments so names fit in a table.
    while True:
        s = re.sub(r"<[^<>]*>", "", symbol)
        s = re.sub(r"\([^()]*\)", "", s)
        if s == symbol:
            return s.strip()
        symbol = s


def perf_top(perf: str, data: Path, children: bool, top: int) -> list[tuple[float, str]]:
    cmd = [perf, "report", "-i", str(data), "--stdio", "-g", "none", "--sort", "symbol", "--percent-limit", "0.5"]
    cmd.append("--children" if children else "--no-children")
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    rows = []
    for line in out.splitlines():
        m = re.match(r"\s*([\d.]+)%\s+(?:([\d.]+)%\s+)?\[([.k])\]\s+(.*)", line)
        name = short(re.sub(r"(\s+-)+\s*$", "", m.group(4))) if m else ""
        # Kernel frames (page faults) only count toward self time; inclusive lists the compiler's own calls.
        if m and name not in STARTUP and not (children and m.group(3) == "k"):
            rows.append((float(m.group(1)), name))
    return rows[:top]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stage0", required=True)
    ap.add_argument("--shapes", default="chain,wide,tree,forest")
    ap.add_argument("--classes", default="200000", help="comma-separated sizes")
    ap.add_argument("--uses", type=int, default=100)
    ap.add_argument("--interfaces", type=int, default=2)
    ap.add_argument("--timeout", type=float, default=1800)
    ap.add_argument("--perf", action="store_true", help="record and report a perf profile per run")
    ap.add_argument("--perf-exe", default=shutil.which("perf") or "perf")
    ap.add_argument("--frequency", type=int, default=999)
    ap.add_argument("--top", type=int, default=15)
    ap.add_argument("--out", help="keep generated sources and perf data here")
    args = ap.parse_args()

    out = Path(args.out) if args.out else Path(tempfile.mkdtemp(prefix="phs-hierarchy-"))
    out.mkdir(parents=True, exist_ok=True)
    failed = False
    print(f"{'shape':<8} {'classes':>8} {'seconds':>9} {'peak MiB':>9}  result")
    for shape in args.shapes.split(","):
        for n in (int(c) for c in args.classes.split(",")):
            source = out / f"{shape}-{n}.phs"
            subprocess.run([sys.executable, str(HERE / "gen_hierarchy.py"), "--shape", shape, "--classes", str(n),
                            "--uses", str(args.uses), "--interfaces", str(args.interfaces), "-o", str(source)], check=True)
            cmd = [args.stage0, "check", str(source)]
            data = out / f"{shape}-{n}.perf.data"
            if args.perf:
                # perf's own memory is small next to the compiler's, so the peak is still the compiler's.
                cmd = [args.perf_exe, "record", "-q", "-g", "-F", str(args.frequency), "-o", str(data), "--", *cmd]
            elapsed, rss, rc = run(cmd, args.timeout)
            result = "timeout" if rc is None else ("ok" if rc == 0 else f"exit {rc}")
            failed |= result != "ok"
            peak = f"{rss / 1024:.0f}" if rss else "-"
            print(f"{shape:<8} {n:>8} {elapsed:>9.2f} {peak:>9}  {result}", flush=True)
            if args.perf and rc is not None:
                for children, title in ((False, "self"), (True, "inclusive")):
                    print(f"  top {title} time:")
                    for pct, name in perf_top(args.perf_exe, data, children, args.top):
                        print(f"    {pct:6.2f}%  {name}")
                print(flush=True)
            if not args.out:
                source.unlink()
    if not args.out:
        shutil.rmtree(out, ignore_errors=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
