#!/usr/bin/env python3
"""Parameter interface coverage check (area 21 R16: no dead parameter hooks).

Collects the function names declared by the parameter interface headers and fails if any of them is
never referenced outside its own declaration and definition files, i.e. has no caller and no test.

Usage: check_param_coverage.py <repo root>
"""
import os
import re
import sys

HEADERS = [
    "core/params/include/amq/params/param_spec.hpp",
    "core/params/include/amq/params/parameter_table.hpp",
    "core/params/include/amq/params/mappings.hpp",
    "ui/include/amq/ui/edit_controller.hpp",
]
DEFINITIONS = {
    "core/params/src/param_specs.cpp",
    "core/params/src/parameter_table.cpp",
    "core/params/src/mappings.cpp",
}
SEARCH_DIRS = ["core", "plugin", "ui", "tests"]
# Declarations: a return type, a name, an opening parenthesis; skip keywords and constructors.
DECL = re.compile(r"^\s*(?:\[\[nodiscard\]\]\s*)?(?:virtual\s+|static\s+|constexpr\s+|inline\s+)*"
                  r"[\w:<>,\s\*&]+?\s+[\*&]?(\w+)\s*\(", re.M)
SKIP = {"operator", "if", "for", "while", "switch", "return", "sizeof", "static_assert"}


def main(root):
    names = {}
    for h in HEADERS:
        text = open(os.path.join(root, h), encoding="utf-8").read()
        text = re.sub(r"//.*", "", text)
        for m in DECL.finditer(text):
            n = m.group(1)
            if n in SKIP or n[0].isupper():
                continue
            names.setdefault(n, h)
    sources = []
    for d in SEARCH_DIRS:
        for dirpath, _, files in os.walk(os.path.join(root, d)):
            if "_deps" in dirpath:
                continue
            for f in files:
                if f.endswith((".cpp", ".hpp", ".h")):
                    rel = os.path.relpath(os.path.join(dirpath, f), root).replace("\\", "/")
                    if rel in HEADERS or rel in DEFINITIONS:
                        continue
                    sources.append(open(os.path.join(dirpath, f), encoding="utf-8", errors="replace").read())
    blob = "\n".join(sources)
    dead = sorted(n for n in names if not re.search(r"\b" + re.escape(n) + r"\b", blob))
    print(f"parameter interface: {len(names)} functions checked")
    if dead:
        for n in dead:
            print(f"  no caller or test: {n} ({names[n]})")
        return 1
    print("  every function has a caller or a test")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "."))
