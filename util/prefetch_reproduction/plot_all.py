#!/usr/bin/env python3
"""Generate the four PDIP reproduction figures from existing result files.

This script never starts gem5. It delegates statistics parsing and plotting to
the individual ``run_fig*.py --skip-run`` entry points.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent
FIGURES = {
    "fig12": ROOT / "run_fig12.py",
    "fig13": ROOT / "run_fig13.py",
    "fig14": ROOT / "run_fig14.py",
    "fig16": ROOT / "run_fig16.py",
}


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Plot PDIP figures from completed gem5 results; never runs gem5."
    )
    parser.add_argument(
        "--figure", choices=("all", *FIGURES), default="all",
        help="Figure to regenerate (default: all four).",
    )
    parser.add_argument(
        "--continue-on-error", action="store_true",
        help="Continue plotting other figures if one has incomplete data.",
    )
    args = parser.parse_args()

    names = list(FIGURES) if args.figure == "all" else [args.figure]
    failures = 0
    for name in names:
        command = [sys.executable, str(FIGURES[name]), "--skip-run"]
        print(f"[plot] {name}: {' '.join(command)}", flush=True)
        completed = subprocess.run(command, cwd=ROOT.parent.parent)
        if completed.returncode:
            failures += 1
            print(
                f"[plot] {name}: skipped/failed (return code {completed.returncode})",
                file=sys.stderr,
                flush=True,
            )
            if not args.continue_on_error:
                return completed.returncode
        else:
            print(f"[plot] {name}: done", flush=True)

    if failures:
        print(f"[plot] completed with {failures} failed figure(s)", file=sys.stderr)
        return 1
    print("[plot] all requested figures generated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
