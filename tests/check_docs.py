#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Documentation checks, run by CTest:

  check_docs.py <cc> <include-dir> <library-file> <file.md>...

CHECK_DOCS_FLAGS holds extra compiler flags (e.g. sanitizers of the build).

1. Every riak_* / RIAK_* identifier mentioned in the Markdown files exists
   in include/riak/riak.h (catches renamed or misspelled API names).
2. Every C code block that defines main() is compiled and linked against the
   library (catches examples that no longer build).
"""

import os
import re
import subprocess
import sys
import tempfile


def main():
    cc, include, lib, docs = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
    header = open(os.path.join(include, "riak", "riak.h"), encoding="utf-8").read()
    known = set(re.findall(r"\b((?:riak|RIAK)_\w+)\b", header))
    # build options, CMake targets and Riak server-side names are not API
    cmake = open(os.path.join(include, "..", "CMakeLists.txt"), encoding="utf-8").read()
    known |= set(re.findall(r"option\((RIAK_\w+)", cmake))
    known |= {"riak_static", "riak_kv", "riak_kv_mapreduce"}
    known |= {"RIAK_TAG"}  # build argument of tests/docker/openriak
    env_prefixes = ("RIAK_TEST_", "RIAK_DOCS_")
    failures = 0
    programs = 0
    for path in docs:
        text = open(path, encoding="utf-8").read()
        for name in sorted(set(re.findall(r"\b((?:riak|RIAK)_\w+)\b", text))):
            if name not in known and not name.endswith("_") and not name.startswith(env_prefixes):
                print(f"{path}: unknown identifier {name}")
                failures += 1
        for block in re.findall(r"```c\n(.*?)```", text, re.S):
            if not re.search(r"\bint\s+main\s*\(", block):
                continue
            programs += 1
            with tempfile.TemporaryDirectory() as tmp:
                src = os.path.join(tmp, "example.c")
                with open(src, "w", encoding="utf-8") as f:
                    f.write(block)
                extra = os.environ.get("CHECK_DOCS_FLAGS", "").split()
                cmd = [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", *extra, "-I", include, src,
                       lib, "-o", os.path.join(tmp, "example")]
                r = subprocess.run(cmd, capture_output=True, text=True)
                if r.returncode != 0:
                    print(f"{path}: example does not build:\n{r.stderr}")
                    failures += 1
    print(f"{len(docs)} files, {programs} complete programs checked, {failures} problems")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
