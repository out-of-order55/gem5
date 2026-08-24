#!/usr/bin/env python3
"""Run and plot PDIP table-associativity sensitivity."""

from __future__ import annotations

import argparse
from pathlib import Path

from common import (PAPER_LABELS, PAPER_ORDER, WORKLOADS, geomean, paper_rc,
                    run_configs, weighted_ipc, write_csv)


ASSOCIATIVITIES = (2, 4, 8, 16)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--skip-run", action="store_true")
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--workload", action="append", choices=WORKLOADS)
    parser.add_argument("--max-weight-only", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2] / "results/prefetch_reproduction/fig13"

    if not args.skip_run:
        for assoc in ASSOCIATIVITIES:
            run_configs(
                root / f"assoc-{assoc}",
                ("fdip", "pdip"),
                pdip_sets=512,
                pdip_assoc=assoc,
                force=args.force,
                dry_run=args.dry_run,
                jobs=args.jobs,
                workloads=args.workload,
                max_weight_only=args.max_weight_only,
            )
        if args.dry_run:
            return

    rows = []
    for assoc in ASSOCIATIVITIES:
        run_root = root / f"assoc-{assoc}"
        for workload in WORKLOADS:
            baseline = weighted_ipc(run_root, workload, "fdip")
            policy = weighted_ipc(run_root, workload, "pdip")
            if baseline is None or policy is None or baseline <= 0:
                continue
            rows.append({
                "assoc": assoc,
                "table_kb": 10.875 * assoc / 2.0,
                "workload": workload,
                "speedup_pct": 100.0 * (policy / baseline - 1.0),
            })
    if not rows:
        raise SystemExit("No completed Figure 13 runs found")
    for assoc in ASSOCIATIVITIES:
        values = [row["speedup_pct"] for row in rows if row["assoc"] == assoc]
        if values:
            rows.append({
                "assoc": assoc,
                "table_kb": 10.875 * assoc / 2.0,
                "workload": "geomean",
                "speedup_pct": 100.0 * (geomean(
                    [1.0 + float(value) / 100.0 for value in values]) - 1.0),
            })
    write_csv(root / "pdip-table-sensitivity.csv", rows)

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    paper_rc()
    fig, ax = plt.subplots(figsize=(3.45, 1.72))
    labels = [name for name in PAPER_ORDER
              if any(row["workload"] == name for row in rows)] + ["geomean"]
    x = list(range(len(labels)))
    width = 0.18
    colors = ("#bcbd22", "#17becf", "#ff7f0e", "#8c564b")
    policy_names = {2: "11", 4: "22", 8: "44", 16: "87"}
    for index, assoc in enumerate(ASSOCIATIVITIES):
        points = {row["workload"]: float(row["speedup_pct"])
                  for row in rows if row["assoc"] == assoc}
        values = [points.get(name, float("nan")) for name in labels]
        ax.bar([value + (index - 1.5) * width for value in x], values, width,
               label=f"PDIP({policy_names[assoc]})", color=colors[index],
               edgecolor="none")
    ax.set_ylabel("Speedup wrt FDIP")
    ax.set_xticks(x, ["geomean" if label == "geomean" else PAPER_LABELS[label]
                      for label in labels], rotation=55, ha="right")
    ax.axhline(0, color="black", linewidth=0.7)
    ax.grid(axis="y", color="#b0b0b0", linewidth=0.55)
    ax.set_axisbelow(True)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.legend(loc="upper center", ncol=4, frameon=False,
              bbox_to_anchor=(0.5, 1.22), columnspacing=0.8)
    fig.tight_layout()
    fig.savefig(root / "pdip-table-sensitivity.png", dpi=180)
    fig.savefig(root / "pdip-table-sensitivity.pdf")


if __name__ == "__main__":
    main()
