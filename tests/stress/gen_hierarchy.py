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
"""Generates large class hierarchies to stress the stage0 checker.

Shapes:
  chain  C1: C0, C2: C1, ... (one deep chain)
  wide   every class derives from C0
  tree   k-ary tree: Ci derives from C((i-1)/k)
  forest many independent chains of --depth classes

Every class overrides one method and adds one field, so member lookup,
override matching and witness checks all walk the hierarchy. --uses adds
top-level expressions that call methods on, read fields of, and upcast
instances of evenly spaced classes. --interfaces makes the root conform to
that many interfaces, so each class has inherited conformances to check.

Usage: python tests/stress/gen_hierarchy.py --shape chain --classes 200000 -o out.phs
"""

from __future__ import annotations

import argparse
import sys


def parent(shape: str, i: int, fanout: int, depth: int) -> int | None:
    if i == 0:
        return None
    if shape == "chain":
        return i - 1
    if shape == "wide":
        return 0
    if shape == "tree":
        return (i - 1) // fanout
    if shape == "forest":
        return None if i % depth == 0 else i - 1
    raise ValueError(shape)


def generate(shape: str, classes: int, fanout: int, depth: int, uses: int, interfaces: int, out) -> None:
    w = out.write
    for k in range(interfaces):
        w(f"interface I{k} {{ fun i{k}(): Int }}\n")
    conform = "".join(f", I{k}" for k in range(interfaces))
    for i in range(classes):
        p = parent(shape, i, fanout, depth)
        if p is None:
            bases = conform[2:] if conform else ""
            head = f"open class C{i}" + (f": {bases}" if bases else "")
            w(f"{head} {{\n    let f{i}: Int = {i}\n    open fun m(): Int = f{i}\n")
            for k in range(interfaces):
                w(f"    fun i{k}(): Int = {k}\n")
            w("}\n")
        else:
            w(f"open class C{i}: C{p} {{\n    let f{i}: Int = {i}\n    open override fun m(): Int = f{i} + super.m()\n}}\n")
    if uses and classes:
        step = max(1, classes // uses)
        for n, i in enumerate(range(classes - 1, -1, -step)):
            if n >= uses:
                break
            root = i
            while (p := parent(shape, root, fanout, depth)) is not None:
                root = p
            w(f"let v{n} = C{i}()\nlet m{n}: Int = v{n}.m()\nlet f{n}: Int = v{n}.f{root}\nlet b{n}: C{root} = v{n}\n")
            if interfaces:
                w(f"let i{n}: I0 = v{n}\n")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--shape", choices=["chain", "wide", "tree", "forest"], default="chain")
    ap.add_argument("--classes", type=int, default=200_000)
    ap.add_argument("--fanout", type=int, default=4, help="children per class for --shape tree")
    ap.add_argument("--depth", type=int, default=100, help="chain length for --shape forest")
    ap.add_argument("--uses", type=int, default=100, help="top-level uses of generated classes")
    ap.add_argument("--interfaces", type=int, default=0)
    ap.add_argument("-o", "--out", default="-")
    args = ap.parse_args()
    out = sys.stdout if args.out == "-" else open(args.out, "w", encoding="utf-8", newline="\n")
    with out:
        generate(args.shape, args.classes, args.fanout, args.depth, args.uses, args.interfaces, out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
