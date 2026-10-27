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
"""Golden-runner regressions for exit status and safe baseline updates."""

from pathlib import Path
import contextlib
import io
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import run_golden


class GoldenRunnerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory(prefix="phs-golden-runner-")
        self.addCleanup(self.directory.cleanup)
        self.source = Path(self.directory.name) / "case[1].phs"
        self.source.write_text("let x = 1\n", encoding="utf-8")
        self.baseline = self.source.with_suffix(".diag")

    def run_case(self, suite, code, expected="", stderr=None, stdout="", update=False):
        self.baseline.write_bytes(expected.encode("utf-8"))
        result = subprocess.CompletedProcess([], code, stdout, expected if stderr is None else stderr)
        with patch.object(run_golden.subprocess, "run", return_value=result):
            return run_golden.run_case(Path("unused-compiler"), suite, self.source, update)

    def test_diagnostics_always_requires_failure(self) -> None:
        failures = self.run_case("diagnostics", 0)
        self.assertTrue(any("expected exit code 1, got 0" in failure for failure in failures))
        self.assertEqual(self.run_case("diagnostics", 1), [])

    def test_update_cannot_turn_negative_case_into_success(self) -> None:
        original = "case[1].phs:1:1: error[E0100]: expected token\n"
        failures = self.run_case("diagnostics", 0, original, stderr="", update=True)
        self.assertTrue(any("expected files not updated" in failure for failure in failures))
        self.assertEqual(self.baseline.read_text(encoding="utf-8"), original)

    def test_semantics_success_requires_zero(self) -> None:
        self.assertEqual(self.run_case("semantics", 0), [])
        failures = self.run_case("semantics", 1)
        self.assertTrue(any("expected exit code 0, got 1" in failure for failure in failures))

    def test_semantics_error_requires_one(self) -> None:
        original = "case[1].phs:12:3: error[E0200]: duplicate binding\n"
        self.assertEqual(self.run_case("semantics", 1, original), [])
        failures = self.run_case("semantics", 0, original)
        self.assertTrue(any("expected exit code 1, got 0" in failure for failure in failures))

    def test_warning_snippets_do_not_change_exit_status(self) -> None:
        warning = (
            "case[1].phs:1:1: warning[E0100]: sample warning\n"
            '    1 | let text = "error[ : error[ case[1].phs:2:3: error[E0200]:"\n'
            "      | ^^^\n"
            "case[1].phs:1:1: note: error[E0200]: this is only a note\n"
        )
        self.assertEqual(self.run_case("semantics", 0, warning), [])
        failures = self.run_case("semantics", 1, warning)
        self.assertTrue(any("expected exit code 0, got 1" in failure for failure in failures))
        self.assertEqual(self.run_case("semantics", 0, stderr=warning, update=True), [])
        self.assertEqual(self.baseline.read_text(encoding="utf-8"), warning)

    def test_check_stdout_prevents_update(self) -> None:
        original = "case[1].phs:1:1: error[E0200]: original\n"
        failures = self.run_case("semantics", 0, original, stderr="", stdout="unexpected", update=True)
        self.assertEqual(failures, ["unexpected stdout; expected files not updated"])
        self.assertEqual(self.baseline.read_text(encoding="utf-8"), original)

    def test_abnormal_exit_prevents_update(self) -> None:
        original = "case[1].phs:1:1: error[E0200]: original\n"
        failures = self.run_case("semantics", 2, original, stderr="", update=True)
        self.assertTrue(any("abnormal exit code 2" in failure for failure in failures))
        self.assertEqual(self.baseline.read_text(encoding="utf-8"), original)

    def test_positive_lexer_parser_failure_cannot_update(self) -> None:
        for suite, extension in (("lexer", ".tokens"), ("parser", ".ast")):
            with self.subTest(suite=suite):
                tree = self.source.with_suffix(extension)
                tree.write_text("original output\n", encoding="utf-8")
                self.baseline.unlink(missing_ok=True)
                result = subprocess.CompletedProcess([], 1, "partial output\n", "case[1].phs:1:1: error[E0100]: broken\n")
                with patch.object(run_golden.subprocess, "run", return_value=result):
                    failures = run_golden.run_case(Path("unused-compiler"), suite, self.source, True)
                self.assertTrue(any("expected files not updated" in failure for failure in failures))
                self.assertEqual(tree.read_text(encoding="utf-8"), "original output\n")
                self.assertFalse(self.baseline.exists())

    def test_explicit_negative_lexer_parser_status(self) -> None:
        error = "case[1].phs:1:1: error[E0100]: expected token\n"
        for suite, extension in (("lexer", ".tokens"), ("parser", ".ast")):
            tree = self.source.with_suffix(extension)
            tree.write_text("original\n", encoding="utf-8")
            self.assertEqual(self.run_case(suite, 1, error, stdout="updated\n", update=True), [])
            self.assertEqual(tree.read_text(encoding="utf-8"), "updated\n")
            failures = self.run_case(suite, 0, error, stdout="updated\n")
            self.assertTrue(any("expected exit code 1, got 0" in failure for failure in failures))
            failures = self.run_case(suite, 0, error, stderr="", stdout="partial\n", update=True)
            self.assertTrue(any("expected files not updated" in failure for failure in failures))
            self.assertEqual(tree.read_text(encoding="utf-8"), "updated\n")
            self.assertEqual(self.baseline.read_text(encoding="utf-8"), error)

    def test_no_cases_is_an_error(self) -> None:
        for filter_text in ([], ["unmatched-filter"]):
            with patch.object(run_golden, "ROOT", Path(self.directory.name)), contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()) as stderr:
                status = run_golden.main(["--stage0", sys.executable, *filter_text])
            self.assertEqual(status, 2)
            self.assertIn("no test cases", stderr.getvalue())

    def test_unmatched_filter_does_not_run_existing_cases(self) -> None:
        root = Path(self.directory.name)
        (root / "parser").mkdir()
        (root / "parser" / "existing.phs").write_text("let x = 1\n", encoding="utf-8")
        with patch.object(run_golden, "ROOT", root), patch.object(run_golden.subprocess, "run") as compiler, contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            status = run_golden.main(["--stage0", sys.executable, "unmatched-filter"])
        self.assertEqual(status, 2)
        compiler.assert_not_called()

    def test_success_status_cannot_introduce_error_baseline(self) -> None:
        error = "case[1].phs:1:1: error[E0100]: broken\n"
        for suite, extension in (("lexer", ".tokens"), ("parser", ".ast")):
            tree = self.source.with_suffix(extension)
            tree.write_text("original\n", encoding="utf-8")
            failures = self.run_case(suite, 0, stderr=error, stdout="partial\n", update=True)
            self.assertTrue(any("expected files not updated" in failure for failure in failures))
            self.assertEqual(tree.read_text(encoding="utf-8"), "original\n")
            self.assertEqual(self.baseline.read_text(encoding="utf-8"), "")

    def run_program(self, status, stderr=b"", stdout=b"", update=False, build=None):
        build = build or subprocess.CompletedProcess([], 0, "", "")
        program = subprocess.CompletedProcess([], status, stdout, stderr)
        with patch.object(run_golden.subprocess, "run", side_effect=[build, program]):
            return run_golden.run_program_case(Path("unused-compiler"), self.source, update)

    def test_run_compares_status_and_stderr(self) -> None:
        exit_file, stderr_file = self.source.with_suffix(".exit"), self.source.with_suffix(".stderr")
        exit_file.write_text("101\n", encoding="utf-8")
        stderr_file.write_text("panic: arithmetic overflow\n", encoding="utf-8")
        self.assertEqual(self.run_program(101, b"panic: arithmetic overflow\n"), [])
        failures = self.run_program(42, b"panic: arithmetic overflow\n")
        self.assertEqual(failures, ["expected exit status 101, got 42"])
        failures = self.run_program(101)
        self.assertTrue(any("stderr differs" in failure for failure in failures))
        failures = self.run_program(101, b"panic: arithmetic overflow\n", stdout=b"extra")
        self.assertTrue(any("stdout differs" in failure for failure in failures))

    def test_run_compares_stdout(self) -> None:
        self.source.with_suffix(".exit").write_text("0\n", encoding="utf-8")
        self.source.with_suffix(".stdout").write_text("hello\n", encoding="utf-8")
        self.assertEqual(self.run_program(0, stdout=b"hello\n"), [])
        failures = self.run_program(0, stdout=b"goodbye\n")
        self.assertTrue(failures and failures[0].startswith("stdout differs"))
        self.assertTrue(any("stdout differs" in failure for failure in self.run_program(0)))

    def test_run_update_writes_sidecars(self) -> None:
        exit_file, stderr_file = self.source.with_suffix(".exit"), self.source.with_suffix(".stderr")
        self.assertEqual(self.run_program(101, b"panic: division by zero\n", update=True), [])
        self.assertEqual(exit_file.read_text(encoding="utf-8"), "101\n")
        self.assertEqual(stderr_file.read_text(encoding="utf-8"), "panic: division by zero\n")
        stdout_file = self.source.with_suffix(".stdout")
        self.assertEqual(self.run_program(101, b"panic: overflow\n", stdout=b"1\n", update=True), [])
        self.assertEqual(stdout_file.read_text(encoding="utf-8"), "1\n")
        self.assertEqual(self.run_program(42, update=True), [])
        self.assertEqual(exit_file.read_text(encoding="utf-8"), "42\n")
        self.assertFalse(stderr_file.exists())
        self.assertFalse(stdout_file.exists())

    def test_run_build_failure_and_signal(self) -> None:
        self.source.with_suffix(".exit").write_text("0\n", encoding="utf-8")
        failed = subprocess.CompletedProcess([], 1, "", "case[1].phs:1:1: error[E0600]: unsupported\n")
        failures = self.run_program(0, build=failed, update=True)
        self.assertTrue(failures and "build failed with exit code 1" in failures[0])
        self.assertEqual(self.source.with_suffix(".exit").read_text(encoding="utf-8"), "0\n")
        self.assertEqual(self.run_program(-11), ["program killed by signal 11"])

    def test_run_suite_is_skipped_off_linux(self) -> None:
        root = Path(self.directory.name)
        (root / "run").mkdir()
        (root / "run" / "program.phs").write_text("fun main() {}\n", encoding="utf-8")
        with patch.object(run_golden, "ROOT", root), patch.object(run_golden, "can_run_programs", return_value=False), patch.object(run_golden.subprocess, "run") as compiler, contextlib.redirect_stdout(io.StringIO()) as stdout:
            status = run_golden.main(["--stage0", sys.executable])
        self.assertEqual(status, 0)
        self.assertIn("1 skipped", stdout.getvalue())
        compiler.assert_not_called()


if __name__ == "__main__":
    unittest.main()
