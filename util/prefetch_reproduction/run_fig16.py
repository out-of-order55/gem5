#!/usr/bin/env python3
"""Run and plot PDIP prefetch-trigger distribution."""

from __future__ import annotations

import argparse
from pathlib import Path

from common import (PAPER_LABELS, PAPER_ORDER, WORKLOADS, paper_rc, run_configs,
                    weighted_metric, write_csv)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--skip-run", action="store_true")
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--workload", action="append", choices=WORKLOADS)
    parser.add_argument("--max-weight-only", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2] / "results/prefetch_reproduction/fig16"
    if not args.skip_run:
        run_configs(root, ("pdip",), pdip_sets=512, pdip_assoc=8,
                    force=args.force, dry_run=args.dry_run,
                    jobs=args.jobs, workloads=args.workload,
                    max_weight_only=args.max_weight_only)
        if args.dry_run:
            return

    rows = []
    for workload in WORKLOADS:
        mispred = weighted_metric(root, workload, "pdip", "issuedResteerTargets")
        last_taken = weighted_metric(root, workload, "pdip", "issuedLastTakenTargets")
        if mispred is None or last_taken is None or mispred + last_taken <= 0:
            continue
        total = mispred + last_taken
        rows.append({
            "workload": workload,
            "mispredicted_target_pct": 100.0 * mispred / total,
            "last_taken_target_pct": 100.0 * last_taken / total,
            "mispredicted_targets": mispred,
            "last_taken_targets": last_taken,
        })
    if not rows:
        raise SystemExit("No completed Figure 16 runs found")
    ordered = [name for name in PAPER_ORDER
               if any(row["workload"] == name for row in rows)]
    rows = [row for name in ordered for row in rows
            if row["workload"] == name]
    if rows:
        rows.append({
            "workload": "geomean",
            "mispredicted_target_pct": sum(row["mispredicted_target_pct"]
                                            for row in rows) / len(rows),
            "last_taken_target_pct": sum(row["last_taken_target_pct"]
                                          for row in rows) / len(rows),
            "mispredicted_targets": "",
            "last_taken_targets": "",
        })
    write_csv(root / "trigger-distribution.csv", rows)

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    paper_rc()
    fig, ax = plt.subplots(figsize=(3.45, 1.75))
    labels = ["geomean" if row["workload"] == "geomean"
              else PAPER_LABELS[row["workload"]] for row in rows]
    mispred = [row["mispredicted_target_pct"] for row in rows]
    last_taken = [row["last_taken_target_pct"] for row in rows]
    ax.bar(labels, mispred, label="Mispredicted Target Triggers",
           color="#1f77b4", edgecolor="none")
    ax.bar(labels, last_taken, bottom=mispred, label="Last Taken Target Triggers",
           color="#d62728", edgecolor="none")
    ax.set_ylabel("Prefetch Trigger Distribution")
    ax.set_ylim(0, 100)
    ax.set_xticks(range(len(labels)), labels, rotation=65, ha="right")
    ax.legend(loc="upper center", ncol=2, frameon=False,
              bbox_to_anchor=(0.5, 1.23), columnspacing=0.7)
    ax.grid(axis="y", color="#b0b0b0", linewidth=0.55)
    ax.set_axisbelow(True)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    fig.tight_layout()
    fig.savefig(root / "trigger-distribution.png", dpi=180)
    fig.savefig(root / "trigger-distribution.pdf")


if __name__ == "__main__":
    main()
