#!/usr/bin/env python3
"""Run and plot FEC-stall reduction for FDIP, PDIP(44), and EIP(46)."""

from __future__ import annotations

import argparse
from pathlib import Path

from common import (PAPER_LABELS, PAPER_ORDER, WORKLOADS, geomean,
                    monitor_metric, paper_rc, run_configs, write_csv)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--skip-run", action="store_true")
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--workload", action="append", choices=WORKLOADS)
    parser.add_argument("--max-weight-only", action="store_true")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[2] / "results/prefetch_reproduction/fig12"
    if not args.skip_run:
        run_configs(
            root,
            ("fdip", "pdip", "eip"),
            pdip_assoc=8,
            fec_monitor=True,
            force=args.force,
            dry_run=args.dry_run,
            jobs=args.jobs,
            workloads=args.workload,
            max_weight_only=args.max_weight_only,
        )
        if args.dry_run:
            return

    rows = []
    for workload in WORKLOADS:
        baseline = monitor_metric(root, workload, "fdip", "decodeStallEvents")
        pdip = monitor_metric(root, workload, "pdip", "decodeStallEvents")
        eip = monitor_metric(root, workload, "eip", "decodeStallEvents")
        if baseline is None or baseline <= 0:
            continue
        rows.append({
            "workload": workload,
            "pdip44_reduction_pct": 100.0 * (baseline - pdip) / baseline
            if pdip is not None else "nan",
            "eip46_reduction_pct": 100.0 * (baseline - eip) / baseline
            if eip is not None else "nan",
            "fdip_fec_stalls": baseline,
            "pdip44_fec_stalls": pdip if pdip is not None else "nan",
            "eip46_fec_stalls": eip if eip is not None else "nan",
        })
    if not rows:
        raise SystemExit("No completed Figure 12 runs found")
    pdip_values = [float(r["pdip44_reduction_pct"]) for r in rows
                   if r["pdip44_reduction_pct"] != "nan"]
    eip_values = [float(r["eip46_reduction_pct"]) for r in rows
                  if r["eip46_reduction_pct"] != "nan"]
    if pdip_values or eip_values:
        rows.append({
            "workload": "geomean",
            "pdip44_reduction_pct": 100.0 * (geomean(
                [1.0 + v / 100.0 for v in pdip_values]) - 1.0)
            if pdip_values else "nan",
            "eip46_reduction_pct": 100.0 * (geomean(
                [1.0 + v / 100.0 for v in eip_values]) - 1.0)
            if eip_values else "nan",
            "fdip_fec_stalls": "",
            "pdip44_fec_stalls": "",
            "eip46_fec_stalls": "",
        })
    write_csv(root / "fec-stall-reduction.csv", rows)

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    paper_order = [name for name in PAPER_ORDER
                   if any(row["workload"] == name for row in rows)]
    rows = ([row for name in paper_order for row in rows
             if row["workload"] == name] +
            [row for row in rows if row["workload"] == "geomean"])
    paper_rc()
    labels = [PAPER_LABELS.get(str(row["workload"]), str(row["workload"]))
              for row in rows]
    x = list(range(len(labels)))
    width = 0.34
    fig, ax = plt.subplots(figsize=(3.45, 2.05))
    pdip_y = [float(row["pdip44_reduction_pct"]) for row in rows]
    eip_y = [float(row["eip46_reduction_pct"]) for row in rows]
    ax.bar([i - width / 2 for i in x], pdip_y, width, label="PDIP(44)",
           color="#ff7f0e", edgecolor="none")
    ax.bar([i + width / 2 for i in x], eip_y, width, label="EIP(46)",
           color="#e377c2", edgecolor="none")
    ax.set_ylabel("% reduction FEC stalls")
    ax.set_xticks(x, labels, rotation=35, ha="right")
    ax.axhline(0, color="black", linewidth=0.7)
    ax.grid(axis="y", color="#b0b0b0", linewidth=0.55)
    ax.set_axisbelow(True)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.legend(loc="upper center", ncol=2, frameon=False,
              bbox_to_anchor=(0.5, 1.18))
    fig.tight_layout()
    fig.savefig(root / "fec-stall-reduction.png", dpi=180)
    fig.savefig(root / "fec-stall-reduction.pdf")


if __name__ == "__main__":
    main()
