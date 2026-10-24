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
"""Regression tests for source-preserving license-header insertion.

Usage: python tests/test_license_check.py
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "license-check" / "license_check.py"
spec = importlib.util.spec_from_file_location("license_check", SCRIPT)
assert spec is not None and spec.loader is not None
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


class LicenseCheckTests(unittest.TestCase):
    def run_checker(self, root: Path, *, fix: bool = False) -> int:
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return checker.main([str(root), *(["--fix"] if fix else [])])

    def assert_preserved(self, preamble: bytes, body: bytes, *, bom: bytes = b"") -> None:
        with tempfile.TemporaryDirectory(prefix="phs-license-test-") as folder:
            root = Path(folder)
            source = root / "example.py"
            original = bom + preamble + body
            source.write_bytes(original)
            before = compile(original, str(source), "exec")
            self.assertEqual(self.run_checker(root), 1)
            self.assertEqual(source.read_bytes(), original)  # Check-only mode cannot modify it.
            self.assertEqual(self.run_checker(root, fix=True), 0)
            fixed = source.read_bytes()
            self.assertTrue(fixed.startswith(bom + preamble))
            self.assertTrue(fixed.endswith(body))
            self.assertEqual(fixed.count(b"SPDX-License-Identifier: Apache-2.0"), 1)
            after = compile(fixed, str(source), "exec")
            self.assertEqual(after.co_consts, before.co_consts)  # No encoding-induced value changes.
            self.assertEqual(self.run_checker(root), 0)
            self.assertEqual(self.run_checker(root, fix=True), 0)
            self.assertEqual(source.read_bytes(), fixed)  # Fixing twice is byte-for-byte idempotent.

    def test_latin1_cookies_survive_fix(self) -> None:
        layouts = [
            [b"# coding: latin-1"],
            [b"# Module comment", b"# coding: latin-1"],
            [b"", b"# coding: latin-1"],
            [b" \t\f", b"# coding: latin-1"],
            [b"#!/usr/bin/env python3", b"# coding: latin-1"],
            [b" \t# coding=latin-1"],
            [b"# Module comment", b"\f\t# -*- coding: latin-1 -*-"],
            [b"# Module comment", b"# vim: set fileencoding=latin-1 :"],
        ]
        for newline in (b"\n", b"\r\n", b"\r"):
            for lines in layouts:
                with self.subTest(newline=newline, lines=lines):
                    preamble = newline.join(lines) + newline
                    body = b"value = 'caf\xe9'" + newline
                    self.assert_preserved(preamble, body)

    def test_utf8_and_bom(self) -> None:
        for bom in (b"", b"\xef\xbb\xbf"):
            for preamble in (b"", b"#!/usr/bin/env python3\n", b"# Module\n# coding: utf-8\n"):
                with self.subTest(bom=bom, preamble=preamble):
                    # U+2028 belongs to the string, not the source line layout.
                    self.assert_preserved(preamble, "value = '\u03b2\u2028x'\n".encode("utf-8"), bom=bom)

    def test_cookie_at_end_of_file(self) -> None:
        for preamble in (b"# coding: latin-1", b"# Module\n# coding: latin-1", b"#!/usr/bin/env python3"):
            with self.subTest(preamble=preamble):
                self.assert_preserved(preamble, b"")

    def test_only_valid_cookie_positions_form_preamble(self) -> None:
        for lines in (
            [],
            ["# coding:"],
            ["value = 1", "# coding: latin-1"],
            ["# Module", "", "# coding: latin-1"],
            ["value = 1 # coding: latin-1"],
        ):
            with self.subTest(lines=lines):
                self.assertEqual(checker.preamble_len(lines), 0)
        self.assertEqual(checker.preamble_len(["# coding: utf-8", "# coding: latin-1"]), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
