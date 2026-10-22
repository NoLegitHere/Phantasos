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
"""Verify (and optionally insert) the Apache-2.0 header in every source file.

Usage:
    python tools/license-check/license_check.py [--fix] [--year YYYY] [ROOT]

Exit status is 1 if any checked file lacks the header (and --fix was not given).
"""

from __future__ import annotations

import argparse
import datetime
import re
import sys
from pathlib import Path

HEADER_TEMPLATE = [
    "Copyright {year} The Phantasos Authors",
    "SPDX-License-Identifier: Apache-2.0",
    "",
    'Licensed under the Apache License, Version 2.0 (the "License");',
    "you may not use this file except in compliance with the License.",
    "You may obtain a copy of the License at",
    "",
    "    http://www.apache.org/licenses/LICENSE-2.0",
    "",
    "Unless required by applicable law or agreed to in writing, software",
    'distributed under the License is distributed on an "AS IS" BASIS,',
    "WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.",
    "See the License for the specific language governing permissions and",
    "limitations under the License.",
]

SLASH = "//"
HASH = "#"
SEMI = ";"

BY_SUFFIX = {
    ".phs": SLASH,
    ".c": SLASH,
    ".h": SLASH,
    ".cc": SLASH,
    ".cpp": SLASH,
    ".hpp": SLASH,
    ".inc": SLASH,
    ".S": SLASH,
    ".s": HASH,
    ".asm": SEMI,
    ".py": HASH,
    ".sh": HASH,
    ".ps1": HASH,
    ".cmake": HASH,
    ".toml": HASH,
    ".yml": HASH,
    ".yaml": HASH,
}

BY_NAME = {
    "CMakeLists.txt": HASH,
    ".gitignore": HASH,
    ".gitattributes": HASH,
    ".editorconfig": HASH,
}

SKIP_DIRS = {".git", "build", "out", ".phantasos", "__pycache__", ".vs", ".vscode", ".cache"}

YEAR_RE = r"\d{4}(?:-\d{4})?"

UTF8_BOM = b"\xef\xbb\xbf"


def comment_style(path: Path) -> str | None:
    if path.name in BY_NAME:
        return BY_NAME[path.name]
    return BY_SUFFIX.get(path.suffix)


def commented(prefix: str, line: str) -> str:
    return f"{prefix} {line}" if line else prefix


def render_header(prefix: str, year: str) -> list[str]:
    return [commented(prefix, line.format(year=year)) for line in HEADER_TEMPLATE]


def header_patterns(prefix: str) -> list[re.Pattern[str]]:
    pats = []
    for line in HEADER_TEMPLATE:
        escaped = re.escape(commented(prefix, line)).replace(re.escape("{year}"), YEAR_RE)
        pats.append(re.compile(f"^{escaped}\\s*$"))
    return pats


def preamble_len(lines: list[str]) -> int:
    """Number of lines that must stay above the header (shebang, encoding cookie)."""
    n = 0
    if lines and lines[0].startswith("#!"):
        n = 1
    if len(lines) > n and re.match(r"^#.*coding[:=]", lines[n]):
        n += 1
    return n


def has_header(lines: list[str], prefix: str) -> bool:
    start = preamble_len(lines)
    pats = header_patterns(prefix)
    if len(lines) < start + len(pats):
        return False
    return all(p.match(lines[start + i]) for i, p in enumerate(pats))


def iter_files(root: Path):
    for path in sorted(root.rglob("*")):
        if any(part in SKIP_DIRS for part in path.relative_to(root).parts):
            continue
        if path.is_file() and comment_style(path) is not None:
            yield path


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("root", nargs="?", default=Path(__file__).resolve().parents[2], type=Path)
    ap.add_argument("--fix", action="store_true", help="insert the header where missing")
    ap.add_argument("--year", default=str(datetime.date.today().year), help="year used by --fix")
    args = ap.parse_args(argv)

    root: Path = args.root.resolve()
    missing: list[Path] = []
    checked = 0
    for path in iter_files(root):
        checked += 1
        prefix = comment_style(path)
        assert prefix is not None
        raw = path.read_bytes()
        # A UTF-8 BOM must stay at the very start of the file, before the header.
        bom = raw.startswith(UTF8_BOM)
        text = raw[len(UTF8_BOM) if bom else 0 :].decode("utf-8", errors="surrogateescape")
        newline = "\r\n" if "\r\n" in text else "\n"
        lines = text.splitlines()
        if has_header(lines, prefix):
            continue
        if not args.fix:
            missing.append(path)
            continue
        pre = preamble_len(lines)
        body = lines[pre:]
        sep = [""] if body and body[0].strip() else []
        new_lines = lines[:pre] + render_header(prefix, args.year) + sep + body
        out = (newline.join(new_lines) + newline).encode("utf-8", errors="surrogateescape")
        path.write_bytes((UTF8_BOM if bom else b"") + out)
        print(f"fixed: {path.relative_to(root).as_posix()}")

    if missing:
        print("Missing Apache-2.0 license header in:", file=sys.stderr)
        for p in missing:
            print(f"  {p.relative_to(root).as_posix()}", file=sys.stderr)
        print("Run: python tools/license-check/license_check.py --fix", file=sys.stderr)
        return 1
    print(f"license-check: {checked} file(s) OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
