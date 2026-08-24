#!/usr/bin/env python3
"""Run and plot IPC speedup across BTB sizes."""

from __future__ import annotations

import argparse
from pathlib import Path

from common import WORKLOADS, geomean, paper_rc, run_configs, weighted_ipc, write_csv


BTBS = (4096, 8192, 16384, 32768, 65536, 131072)
POLICIES = (
    ("pdip11", "pdip", 2, "lru"),
    ("pdip44", "pdip", 8, "lru"),
    ("eip46", "eip", 8, "lru"),
    ("pdip44-emissary", "pdip", 8, "emissary"),
)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--skip-run", action="store_true")
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--workload", action="append", choices=WORKLOADS)
    parser.add_argument("--max-weight-only", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2] / "results/prefetch_reproduction/fig14"

    if not args.skip_run:
        for btb in BTBS:
            run_configs(
                root / f"btb-{btb}",
                ("fdip",),
                btb_entries=btb,
                force=args.force,
                dry_run=args.dry_run,
                jobs=args.jobs,
                workloads=args.workload,
                max_weight_only=args.max_weight_only,
            )
            for name, config, assoc, l2_rp in POLICIES:
                run_configs(
                    root / f"btb-{btb}" / name,
                    (config,),
                    btb_entries=btb,
                    pdip_assoc=assoc,
                    l2_rp=l2_rp,
                    force=args.force,
                    dry_run=args.dry_run,
                    jobs=args.jobs,
                    workloads=args.workload,
                    max_weight_only=args.max_weight_only,
                )
        if args.dry_run:
            return

    rows = []
    for btb in BTBS:
        base_root = root / f"btb-{btb}"
        for workload in WORKLOADS:
            baseline = weighted_ipc(base_root, workload, "fdip")
            if baseline is None or baseline <= 0:
                continue
            for name, _config, _assoc, _l2_rp in POLICIES:
                policy = weighted_ipc(base_root / name, workload, "pdip" if "pdip" in name else "eip")
                if policy is None:
                    continue
                rows.append({
                    "btb_entries": btb,
                    "policy": name,
                    "workload": workload,
                    "speedup_pct": 100.0 * (policy / baseline - 1.0),
                })
    if not rows:
        raise SystemExit("No completed Figure 14 runs found")
    write_csv(root / "btb-sensitivity.csv", rows)

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    paper_rc()
    fig, ax = plt.subplots(figsize=(3.45, 1.75))
    policy_style = {
        "eip46": ("EIP(46)", "#9467bd", "s"),
        "pdip11": ("PDIP(11)", "#bcbd22", "^"),
        "pdip44": ("PDIP(44)", "#ff7f0e", "o"),
        "pdip44-emissary": ("PDIP(44) + EMISSARY", "#8c564b", "D"),
    }
    for policy in ("eip46", "pdip11", "pdip44", "pdip44-emissary"):
        label, color, marker = policy_style[policy]
        points = [row for row in rows if row["policy"] == policy]
        values = []
        for btb in BTBS:
            selected = [row for row in points if row["btb_entries"] == btb]
            ratios = [1.0 + float(row["speedup_pct"]) / 100.0
                      for row in selected if float(row["speedup_pct"]) > -100.0]
            values.append(100.0 * (geomean(ratios) - 1.0)
                          if ratios else float("nan"))
        ax.plot([btb // 1024 for btb in BTBS], values,
                marker=marker, markersize=3.5, linewidth=1.1,
                label=label, color=color)
    ax.set_xlabel("Number of BTB entries")
    ax.set_ylabel("% IPC gain at respective BTB baseline")
    ax.set_xticks([4, 8, 16, 32, 64, 128])
    ax.axhline(0, color="black", linewidth=0.7)
    ax.grid(color="#b0b0b0", linewidth=0.55)
    ax.set_axisbelow(True)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.legend(loc="upper center", ncol=4, frameon=False,
              bbox_to_anchor=(0.5, 1.24), columnspacing=0.6)
    fig.tight_layout()
    fig.savefig(root / "btb-sensitivity.png", dpi=180)
    fig.savefig(root / "btb-sensitivity.pdf")


if __name__ == "__main__":
    main()
