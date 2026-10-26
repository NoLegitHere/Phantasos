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
"""Golden-file test runner for the stage0 compiler.

Layout (relative to tests/stage0):
    lexer/*.phs        -> `lex`,   stdout compared with *.tokens
    parser/*.phs       -> `parse`, stdout compared with *.ast
    diagnostics/*.phs  -> `check`, stderr compared with *.diag (must fail)
    semantics/*.phs    -> `check`, stderr compared with *.diag (may succeed)
    codegen/*.phs      -> `gate`,  stderr compared with *.diag (may succeed)
    mir/*.phs          -> `mir`,   stdout compared with *.mir

For lexer/parser/mir tests, stderr is compared with *.diag when that file exists;
otherwise the test must succeed with empty stderr.

Usage: python tests/run_golden.py --stage0 <path-to-phantasos-stage0> [--update] [FILTER]
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent / "stage0"
SUITES = {
    "lexer": ("lex", ".tokens"),
    "parser": ("parse", ".ast"),
    "diagnostics": ("check", None),
    "semantics": ("check", None),
    "codegen": ("gate", None),
    "mir": ("mir", ".mir"),
}
CHECK_SUITES = ("diagnostics", "semantics", "codegen")


def norm(text: str) -> str:
    return text.replace("\r\n", "\n")


def check_exit_code(suite: str, src: Path, diagnostics: str) -> int:
    if suite == "diagnostics":
        return 1  # This suite consists exclusively of negative tests.
    # Match a complete header for this source, never a quoted snippet or a note.
    header = rf"^{re.escape(src.name)}:[0-9]+:[0-9]+: error\[E[0-9]+\]:"
    return int(re.search(header, diagnostics, re.MULTILINE) is not None)


def run_case(exe: Path, suite: str, src: Path, update: bool) -> list[str]:
    cmd, out_ext = SUITES[suite]
    proc = subprocess.run(
        [str(exe), cmd, src.name], cwd=src.parent, capture_output=True, text=True, encoding="utf-8"
    )
    stdout, stderr = norm(proc.stdout), norm(proc.stderr)
    failures: list[str] = []

    expected_files: list[tuple[Path, str]] = []
    if out_ext:
        expected_files.append((src.with_suffix(out_ext), stdout))
    diag_file = src.with_suffix(".diag")
    if suite in CHECK_SUITES or diag_file.exists() or (update and stderr):
        expected_files.append((diag_file, stderr))

    if update:
        if proc.returncode not in (0, 1):
            return [f"abnormal exit code {proc.returncode}; expected files not updated"]
        if suite in CHECK_SUITES:
            expected_code = check_exit_code(suite, src, stderr)
            if proc.returncode != expected_code:
                return [f"expected exit code {expected_code}, got {proc.returncode}; expected files not updated"]
            if stdout:
                return ["unexpected stdout; expected files not updated"]
        else:
            # Positive lexer/parser fixtures cannot become negative merely by
            # updating after a compiler regression. Negative cases opt in with
            # an existing primary error header in their diagnostic baseline.
            previous = norm(diag_file.read_text(encoding="utf-8")) if diag_file.exists() else ""
            expected_code = check_exit_code(suite, src, previous)
            if proc.returncode != expected_code:
                return [f"expected exit code {expected_code}, got {proc.returncode}; expected files not updated"]
            if check_exit_code(suite, src, stderr) != expected_code:
                return ["diagnostics do not match expected exit status; expected files not updated"]
        for path, actual in expected_files:
            path.write_text(actual, encoding="utf-8", newline="\n")
        return failures

    for path, actual in expected_files:
        if not path.exists():
            failures.append(f"missing expected file {path.name}")
            continue
        expected = norm(path.read_text(encoding="utf-8"))
        if expected != actual:
            failures.append(f"{path.name} differs:\n{diff(expected, actual)}")

    # 0 (success) and 1 (diagnostics reported) are the only normal statuses;
    # anything else is a crash or infrastructure failure.
    if proc.returncode not in (0, 1):
        failures.append(f"abnormal exit code {proc.returncode}")
    elif suite in CHECK_SUITES:
        if stdout:
            failures.append(f"unexpected stdout:\n{stdout}")
        if diag_file.exists():
            expected_code = check_exit_code(suite, src, diag_file.read_text(encoding="utf-8"))
            if proc.returncode != expected_code:
                failures.append(f"expected exit code {expected_code}, got {proc.returncode}")
    else:
        has_diag = diag_file.exists()
        if not has_diag and stderr:
            failures.append(f"unexpected diagnostics:\n{stderr}")
        expected_code = check_exit_code(suite, src, diag_file.read_text(encoding="utf-8") if has_diag else "")
        if proc.returncode in (0, 1) and proc.returncode != expected_code:
            failures.append(f"expected exit code {expected_code}, got {proc.returncode}")
    return failures


def diff(expected: str, actual: str) -> str:
    import difflib

    lines = difflib.unified_diff(
        expected.splitlines(), actual.splitlines(), "expected", "actual", lineterm="", n=2
    )
    return "\n".join(list(lines)[:60])


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stage0", required=True, type=Path, help="path to phantasos-stage0")
    ap.add_argument("--update", action="store_true", help="rewrite expected files from actual output")
    ap.add_argument("filter", nargs="?", default="", help="only run tests whose path contains this text")
    args = ap.parse_args(argv)

    exe: Path = args.stage0.resolve()
    if not exe.exists() and exe.with_suffix(".exe").exists():
        exe = exe.with_suffix(".exe")
    if not exe.exists():
        print(f"error: stage0 executable not found: {exe}", file=sys.stderr)
        return 2

    total = failed = 0
    for suite in SUITES:
        for src in sorted((ROOT / suite).glob("*.phs")):
            rel = src.relative_to(ROOT).as_posix()
            if args.filter not in rel:
                continue
            total += 1
            failures = run_case(exe, suite, src, args.update)
            if failures:
                failed += 1
                print(f"FAIL {rel}")
                for f in failures:
                    print("  " + f.replace("\n", "\n  "))
            else:
                print(f"ok   {rel}")

    if total == 0:
        print(f"error: no test cases matched filter {args.filter!r}", file=sys.stderr)
        return 2
    verb = "updated" if args.update else "passed"
    print(f"\n{total - failed}/{total} {verb}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
