#!/usr/bin/env python3
"""Diff two lcov tracefiles and print a GitHub-flavored Markdown summary.

Used by the kunit-coverage-diff CI job to compare a PR's coverage.info
against the latest master run's, since lcov's own --diff wants a patch file
rather than two tracefiles, and genhtml's differential mode produces an
HTML tree, not a PR-comment-sized summary.

Scoped to /fs/ (this repo's actual test target) and filtered to changes of
at least NOISE_FLOOR_PP: most of the tracefile's coverage comes from the
xfstests loopback suite doing real TCP/RPC I/O plus upstream
CONFIG_KUNIT_ALL_TESTS suites outside /fs entirely, and their branch
coverage jitters between runs with wall-clock timing and (for some
upstream suites) randomized inputs -- none of which this repo's own tests
can move. An unscoped, unfiltered diff is mostly that jitter.

Usage: coverage-diff.py <base.info> <head.info>
"""

import sys

NOISE_FLOOR_PP = 1.0


def parse_lcov(path):
    """Return {file: (lines_hit, lines_total)} from an lcov tracefile, /fs/ only.

    Keyed on the path after /linux/, not the absolute SF: path: base and
    head can come from different machines (a local VM vs. a GitHub Actions
    runner) with different fetched-tree roots, and matching on the full
    path would make every file look removed-and-re-added.
    """
    files = {}
    cur = None
    keep = False
    hit = total = 0
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("SF:"):
                cur = line[3:].split("/linux/", 1)[-1]
                keep = cur.startswith("fs/")
                hit = total = 0
            elif keep and line.startswith("DA:"):
                total += 1
                if int(line[3:].split(",")[1]) > 0:
                    hit += 1
            elif line == "end_of_record" and keep and cur is not None:
                files[cur] = (hit, total)
                cur = None
    return files


def pct(hit, total):
    return 100.0 * hit / total if total else 0.0


def main():
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} <base.info> <head.info>")
    base = parse_lcov(sys.argv[1])
    head = parse_lcov(sys.argv[2])

    base_hit = sum(h for h, _ in base.values())
    base_total = sum(t for _, t in base.values())
    head_hit = sum(h for h, _ in head.values())
    head_total = sum(t for _, t in head.values())

    print("### KUnit coverage vs `master` (/fs/ only)\n")
    print(
        f"Overall: {pct(base_hit, base_total):.1f}% -> "
        f"{pct(head_hit, head_total):.1f}% "
        f"({head_hit}/{head_total} lines, "
        f"{'+' if head_total >= base_total else ''}{head_total - base_total} lines instrumented)\n"
    )

    # Per-file delta, base_pct/head_pct None means the file is new/removed.
    # A file dropping out of /fs/ entirely (b or h missing) is itself
    # noise-prone -- a file with a handful of instrumented lines can flip
    # in or out of the tracefile between runs -- so that case gets the same
    # NOISE_FLOOR_PP treatment as a percentage change, not an automatic row.
    rows = []
    for f in sorted(set(base) | set(head)):
        b = base.get(f)
        h = head.get(f)
        b_pct = pct(*b) if b else None
        h_pct = pct(*h) if h else None
        delta = (h_pct if h_pct is not None else 0.0) - (b_pct if b_pct is not None else 0.0)
        if abs(delta) < NOISE_FLOOR_PP:
            continue
        rows.append((delta, f, b, h, b_pct, h_pct))

    if not rows:
        print(f"No /fs/ file changed coverage by {NOISE_FLOOR_PP:.1f}pp or more.")
        return

    # Biggest regressions first, then biggest improvements, so a shrinking
    # PR comment (GitHub truncates long ones) keeps the bad news visible.
    rows.sort(key=lambda r: r[0])

    print("| File | Base | Head | Delta |")
    print("|---|---:|---:|---:|")
    for delta, f, b, h, b_pct, h_pct in rows:
        b_s = f"{b_pct:.1f}%" if b_pct is not None else "-"
        h_s = f"{h_pct:.1f}%" if h_pct is not None else "removed"
        sign = "+" if delta >= 0 else ""
        print(f"| `{f}` | {b_s} | {h_s} | {sign}{delta:.1f}pp |")


if __name__ == "__main__":
    main()
