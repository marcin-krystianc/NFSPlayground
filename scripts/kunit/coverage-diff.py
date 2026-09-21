#!/usr/bin/env python3
"""Diff two lcov tracefiles and print a GitHub-flavored Markdown summary.

Used by the kunit-coverage-diff CI job to compare a PR's coverage.info
against the latest master run's, since lcov's own --diff wants a patch file
rather than two tracefiles, and genhtml's differential mode produces an
HTML tree, not a PR-comment-sized summary.

Usage: coverage-diff.py <base.info> <head.info>
"""

import sys


def parse_lcov(path):
    """Return {file: (lines_hit, lines_total)} from an lcov tracefile."""
    files = {}
    cur = None
    hit = total = 0
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("SF:"):
                cur = line[3:]
                hit = total = 0
            elif line.startswith("DA:"):
                total += 1
                if int(line[3:].split(",")[1]) > 0:
                    hit += 1
            elif line == "end_of_record" and cur is not None:
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

    print("### KUnit coverage vs `master`\n")
    print(
        f"Overall: {pct(base_hit, base_total):.1f}% -> "
        f"{pct(head_hit, head_total):.1f}% "
        f"({head_hit}/{head_total} lines, "
        f"{'+' if head_total >= base_total else ''}{head_total - base_total} lines instrumented)\n"
    )

    # Per-file delta, base_pct None means the file is new in this run.
    rows = []
    for f in sorted(set(base) | set(head)):
        b = base.get(f)
        h = head.get(f)
        b_pct = pct(*b) if b else None
        h_pct = pct(*h) if h else None
        delta = (h_pct if h_pct is not None else 0.0) - (b_pct if b_pct is not None else 0.0)
        if b is not None and h is not None and abs(delta) < 0.05:
            continue  # unchanged, don't clutter the table
        rows.append((delta, f, b, h, b_pct, h_pct))

    if not rows:
        print("No per-file coverage change.")
        return

    # Biggest regressions first, then biggest improvements, so a shrinking
    # PR comment (GitHub truncates long ones) keeps the bad news visible.
    rows.sort(key=lambda r: r[0])

    print("| File | Base | Head | Delta |")
    print("|---|---:|---:|---:|")
    for delta, f, b, h, b_pct, h_pct in rows:
        short = f.split("/linux/", 1)[-1]  # drop the fetched-tree prefix
        b_s = f"{b_pct:.1f}%" if b_pct is not None else "-"
        h_s = f"{h_pct:.1f}%" if h_pct is not None else "removed"
        sign = "+" if delta >= 0 else ""
        print(f"| `{short}` | {b_s} | {h_s} | {sign}{delta:.1f}pp |")


if __name__ == "__main__":
    main()
