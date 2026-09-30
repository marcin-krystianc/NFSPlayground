#!/usr/bin/env python3
"""Prepare one lcov tracefile for merging with the others in coverage.yml.

Keeps only records for files git tracks in the kernel tree, and names the
test (lcov's TN:) after where the coverage came from.

Tracking is the test for "product source": what else appears in a
tracefile is the KUnit runner's copied-in test files and fixture (untracked
copies from kunit/), and generated files under the build directories. Those
are not code under test, and a fresh checkout of the same commit, which is
what genhtml renders the merged report from, does not have them.

Paths are matched after "/linux/", as in scripts/kunit/coverage-diff.py, and
the tree's own absolute path is written back, so the output points at the
tree genhtml will read.

Usage: coverage-filter.py <linux-tree> <test-name> <in.info> <out.info>
"""

import os
import subprocess
import sys


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__.strip().splitlines()[-1])
    tree, name, src, dst = sys.argv[1:]
    tree = os.path.abspath(tree)
    tracked = set(subprocess.run(["git", "-C", tree, "ls-files"], check=True,
                                 capture_output=True, text=True).stdout.splitlines())

    kept = dropped = 0
    record = []
    with open(src) as f, open(dst, "w") as out:
        for line in f:
            record.append(line)
            if line.rstrip("\n") != "end_of_record":
                continue
            sf = next((l[3:].rstrip("\n") for l in record if l.startswith("SF:")), "")
            rel = sf.split("/linux/", 1)[1] if "/linux/" in sf else ""
            if rel in tracked:
                for l in record:
                    if l.startswith("TN:"):
                        l = f"TN:{name}\n"
                    elif l.startswith("SF:"):
                        l = f"SF:{tree}/{rel}\n"
                    out.write(l)
                kept += 1
            else:
                dropped += 1
            record = []
    print(f"{src}: kept {kept} files, dropped {dropped} (untracked or generated)")


if __name__ == "__main__":
    main()
