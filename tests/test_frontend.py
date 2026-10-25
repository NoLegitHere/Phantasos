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
"""Deterministic frontend regressions: input limits, nesting, dumps and parser state.

Usage: python tests/test_frontend.py --stage0 <exe>
"""

from __future__ import annotations

import argparse
from collections import Counter
import subprocess
import tempfile
import unittest
from pathlib import Path


class FrontendTests(unittest.TestCase):
    exe: Path

    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory(prefix="phs-regression-")
        self.addCleanup(self.directory.cleanup)
        self.source = Path(self.directory.name) / "case.phs"

    def run_source(self, text: str, command: str = "parse", line_counts: dict[str, int] | None = None):
        self.source.write_text(text, encoding="utf-8")
        # Deep, flat trees produce large indentation. Keep their output out of
        # the test runner's memory while still checking for abnormal exits.
        with tempfile.TemporaryFile() as output:
            result = subprocess.run(
                [str(self.exe), command, str(self.source)],
                stdout=output if line_counts is not None else subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=30,
            )
            self.assertIn(result.returncode, (0, 1), f"abnormal exit: {result.returncode:#x}")
            stderr = result.stderr.decode("utf-8").replace("\r\n", "\n")
            self.assertNotIn("AddressSanitizer", stderr)
            self.assertNotIn("runtime error:", stderr)
            self.assertNotIn("LeakSanitizer", stderr)
            stdout = "" if line_counts is not None else result.stdout.decode("utf-8").replace("\r\n", "\n")
            if line_counts is not None:
                output.seek(0)
                actual = Counter(line.strip().decode("utf-8") for line in output)
                for line, count in line_counts.items():
                    self.assertEqual(actual[line], count, f"incomplete AST dump: {line}")
            return result.returncode, stdout, stderr

    def test_source_failures(self) -> None:
        # Reject oversized files from metadata before allocating or lexing them.
        for oversized in (False, True):
            with self.subTest(oversized=oversized):
                if oversized:
                    with self.source.open("wb") as source:
                        source.truncate(64 * 1024 * 1024 + 1)
                result = subprocess.run(
                    [str(self.exe), "check", str(self.source)],
                    capture_output=True, timeout=10,
                )
                self.assertEqual(result.returncode, 2)
                self.assertEqual(result.stdout, b"")
                self.assertIn(b"error: cannot read", result.stderr)
                if oversized:
                    self.assertIn(b"source file exceeds input limit of 67108864 bytes", result.stderr)

    def test_nesting_limit(self) -> None:
        n = 2000
        cases = {
            "parentheses": "let x = " + "(" * n + "1" + ")" * n,
            "unary": "let x = " + "-" * n + "1",
            "coalesce": "let x = " + " ?? ".join(["x"] * n),
            "prefix_type": "type T = " + "*" * n + "Int",
            "generic_type": "type T = " + "Box<" * n + "Int" + ">" * n,
            "speculative_generic": "let x = f<" + "Box<" * n + "Int" + ">" * n + ">()",
            "pattern": "fun f(x: E) = match x { " + "Case(" * n + "_" + ")" * n + " -> 1 }",
            "declaration": "class C { " * n + "}" * n,
            "block": "fun f() { " + "loop { " * n + "}" * n + "}",
            "else_if": "let x = " + "if true {} else " * n + "{}",
            "mixed": "let x = " + "if true { return " * n + "1" + " }" * n,
        }
        for name, source in cases.items():
            for command in ("check", "parse"):
                with self.subTest(name=name, command=command):
                    code, _, stderr = self.run_source(source + "\n", command)
                    self.assertEqual(code, 1, stderr)
                    self.assertEqual(stderr.count("error["), 1, stderr)
                    self.assertIn("error[E0112]: parser nesting limit (128) exceeded", stderr)

    def test_depth_restored_between_siblings(self) -> None:
        source = "\n".join(f"let x{i} = " + "(" * 40 + "1" + ")" * 40 for i in range(300))
        code, _, stderr = self.run_source(source, "check")
        self.assertEqual((code, stderr), (0, ""))

    def test_check_runs_semantics_after_successful_parse(self) -> None:
        source = "fun f() { break; let x = 1; let x = 2 }"
        code, stdout, stderr = self.run_source(source)
        self.assertEqual((code, stderr), (0, ""))
        self.assertIn("Break", stdout)
        code, stdout, stderr = self.run_source(source, "check")
        self.assertEqual((code, stdout), (1, ""))
        self.assertEqual(stderr.count("error["), 2, stderr)
        self.assertIn("error[E0200]", stderr)
        self.assertIn("error[E0203]", stderr)
        self.assertIn("note: first declared here", stderr)
        # A syntax error suppresses the semantic pass on the recovered tree.
        code, stdout, stderr = self.run_source(source + "\nlet broken = ;", "check")
        self.assertEqual((code, stdout), (1, ""))
        self.assertIn("error[E0101]", stderr)
        self.assertNotIn("error[E02", stderr)

    def test_semantics_handles_long_flat_trees(self) -> None:
        source = "enum E { Case(Int) }\nlet x = 1\nlet sum = " + "+".join(["1"] * 20000)
        source += "\nlet chain = x" + ".member" * 20000
        source += "\nfun f(x: E) = match x { " + " | ".join([".Case(a)"] * 2000) + " -> a }"
        code, stdout, stderr = self.run_source(source, "check")
        self.assertEqual((code, stdout, stderr), (0, "", ""))

    def test_scalar_checking_and_phase_gating(self) -> None:
        source = "let x: Bool = 1\nfun f(): Int { return }\n"
        code, _, stderr = self.run_source(source, "parse")
        self.assertEqual((code, stderr), (0, ""))
        code, stdout, stderr = self.run_source(source, "check")
        self.assertEqual((code, stdout), (1, ""))
        self.assertEqual(stderr.count("error[E0400]"), 2, stderr)
        for invalid, expected in (("fun bad() { break }", "E0203"), ("let missing = unknown", "E0300")):
            code, _, stderr = self.run_source(source + invalid, "check")
            self.assertEqual(code, 1)
            self.assertIn(f"error[{expected}]", stderr)
            self.assertNotIn("error[E04", stderr)

    def test_call_checking_is_not_part_of_parse(self) -> None:
        source = "fun f(x: Int) = x\nlet wrong = f(true)\nlet notCallable = 1()"
        code, _, stderr = self.run_source(source, "parse")
        self.assertEqual((code, stderr), (0, ""))
        code, stdout, stderr = self.run_source(source, "check")
        self.assertEqual((code, stdout), (1, ""))
        self.assertEqual(stderr.count("error["), 2, stderr)
        self.assertIn("error[E0411]", stderr)
        self.assertIn("error[E0410]", stderr)
        code, _, stderr = self.run_source(source + "\nlet invalid = unknown", "check")
        self.assertEqual(code, 1)
        self.assertIn("error[E0300]", stderr)
        self.assertNotIn("error[E04", stderr)

    def test_numeric_conversion_diagnostics(self) -> None:
        source = "let source = 1\nlet narrow = Int8(source)\nlet operand = Int64(true)\nlet shape = Int8.checked()"
        code, _, stderr = self.run_source(source, "parse")
        self.assertEqual((code, stderr), (0, ""))
        code, stdout, stderr = self.run_source(source, "check")
        self.assertEqual((code, stdout), (1, ""))
        self.assertEqual(stderr.count("error["), 3, stderr)
        for code in ("E0420", "E0421", "E0422"):
            self.assertIn(f"error[{code}]", stderr)
        code, _, stderr = self.run_source(source + "\nlet invalid = unknown", "check")
        self.assertEqual(code, 1)
        self.assertIn("error[E0300]", stderr)
        self.assertNotIn("error[E04", stderr)

    def test_alias_expansion_and_cycle_gating(self) -> None:
        source = "type A = B\ntype B = A\nlet wrong: Bool = 1"
        code, _, stderr = self.run_source(source, "parse")
        self.assertEqual((code, stderr), (0, ""))
        code, stdout, stderr = self.run_source(source, "check")
        self.assertEqual((code, stdout), (1, ""))
        self.assertEqual(stderr.count("error["), 1, stderr)
        self.assertIn("error[E0430]", stderr)
        self.assertNotIn("error[E0400]", stderr)
        code, _, stderr = self.run_source("type Small = Int8\nlet wrong: Small = true", "check")
        self.assertEqual(code, 1)
        self.assertIn("error[E0400]: expected 'Int8', found 'Bool'", stderr)

    def test_composite_and_nominal_checks(self) -> None:
        source = "let index = [1][false]\nabstract class A {}\nlet object = A()"
        code, _, stderr = self.run_source(source, "parse")
        self.assertEqual((code, stderr), (0, ""))
        code, stdout, stderr = self.run_source(source, "check")
        self.assertEqual((code, stdout), (1, ""))
        self.assertEqual(stderr.count("error["), 2, stderr)
        self.assertIn("error[E0440]", stderr)
        self.assertIn("error[E0441]", stderr)
        code, _, stderr = self.run_source(source + "\nlet invalid = unknown", "check")
        self.assertEqual(code, 1)
        self.assertIn("error[E0300]", stderr)
        self.assertNotIn("error[E04", stderr)

    def test_check_resolves_names_after_validation(self) -> None:
        source = "fun f(x: Missing) = unknown(x)\n"
        code, _, stderr = self.run_source(source, "parse")
        self.assertEqual((code, stderr), (0, ""))
        code, stdout, stderr = self.run_source(source, "check")
        self.assertEqual((code, stdout), (1, ""))
        self.assertEqual(stderr.count("error["), 2, stderr)
        self.assertIn("error[E0301]: unknown type 'Missing'", stderr)
        self.assertIn("error[E0300]: unknown name 'unknown'", stderr)
        code, _, stderr = self.run_source(source + "fun invalid() { break }", "check")
        self.assertEqual(code, 1)
        self.assertIn("error[E0203]", stderr)
        self.assertNotIn("error[E03", stderr)

    def test_long_flat_expressions(self) -> None:
        for name, (source, counts) in {
            "addition": ("let x = " + "+".join(["1"] * 10000), {"Int 1": 10000, "Binary Add": 9999}),
            "postfix": ("let x = a" + ".b" * 10000, {"Member .b": 10000, "Name a": 1}),
        }.items():
            with self.subTest(name=name):
                code, _, stderr = self.run_source(source, line_counts=counts)
                self.assertEqual((code, stderr), (0, ""))

    def test_long_single_line_token_dump(self) -> None:
        # A generous process timeout catches quadratic prefix rescanning without
        # relying on timing ratios or a machine-specific microbenchmark budget.
        count = 100000
        prefix = "/* \u03b2 */"
        code, stdout, stderr = self.run_source(prefix + "x " * count, "lex")
        self.assertEqual((code, stderr), (0, ""))
        lines = stdout.splitlines()
        self.assertEqual(len(lines), count + 1)
        for i, line in enumerate(lines[:-1]):
            self.assertEqual(line, f"1:{len(prefix) + 1 + 2 * i} Ident x (sp)")
        self.assertEqual(lines[-1], f"1:{len(prefix) + 1 + 2 * count} Eof")

    def test_long_optional_type(self) -> None:
        code, stdout, stderr = self.run_source("type T = Int" + " ?" * 10000)
        self.assertEqual((code, stderr), (0, ""))
        self.assertEqual(stdout, "File\n  TypeAlias T = Int" + "?" * 10000 + "\n")

    def test_speculation_restores_split_tokens(self) -> None:
        code, stdout, stderr = self.run_source("let x = a < b >> c\nlet y = Box<Box<Int>>()\n")
        self.assertEqual((code, stderr), (0, ""))
        self.assertEqual(stdout, """File
  Let x
    Binary Lt
      Name a
      Binary Shr
        Name b
        Name c
  Let y
    Call
      GenericInst <Box<Int>>
        Name Box
""")

    def test_cast_state_and_speculation(self) -> None:
        source = """let a = x as Int < limit
let b = x as Box<Int> < limit
let c = f<Box<Box<Int>>>()
let d = 1 + 2 * 3
"""
        code, stdout, stderr = self.run_source(source)
        self.assertEqual((code, stderr), (0, ""))
        self.assertEqual(stdout.count("Binary Lt"), 2)
        self.assertIn("Cast as Box<Int>", stdout)
        self.assertIn("GenericInst <Box<Box<Int>>>", stdout)
        self.assertIn("Binary Add\n      Int 1\n      Binary Mul\n        Int 2\n        Int 3", stdout)

    def test_match_arm_state_restored_after_block(self) -> None:
        source = """fun f(x: E) = match x {
    .A -> if true { return builder
        .add(1)
    } else { return builder
        .add(2)
    }
    .B -> match x {
        .C -> 3
        .D -> 4
    }
    .E -> 5
}
"""
        code, stdout, stderr = self.run_source(source)
        self.assertEqual((code, stderr), (0, ""))
        for name in "ABCDE":
            self.assertIn(f"Arm .{name}\n", stdout)
        self.assertEqual(stdout.count("Member .add"), 2)

    def test_error_reporting_after_failed_speculation(self) -> None:
        code, _, stderr = self.run_source("let x = a < b >> c\nlet broken = ;\nlet tail = 1\n", "check")
        self.assertEqual(code, 1)
        self.assertEqual(stderr.count("error["), 1, stderr)
        self.assertIn("error[E0101]", stderr)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage0", required=True, type=Path)
    args = parser.parse_args()
    exe = args.stage0.resolve()
    if not exe.exists() and exe.with_suffix(".exe").exists():
        exe = exe.with_suffix(".exe")
    FrontendTests.exe = exe
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(FrontendTests)
    return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
