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
import subprocess
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


if __name__ == "__main__":
    unittest.main()
