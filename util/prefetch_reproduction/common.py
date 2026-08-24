#!/usr/bin/env python3
"""Shared runner and SimPoint/statistics helpers for the PDIP figures."""

from __future__ import annotations

import csv
import json
import math
import subprocess
from pathlib import Path
from typing import Iterable


GEM5_ROOT = Path(__file__).resolve().parents[2]
BENCH_ROOT = Path("/data1/GB/gem5-svr-bench")
GEM5_BIN = GEM5_ROOT / "build/X86/gem5.fast"
RUNNER = BENCH_ROOT / "scripts/run-pdip-workloads.py"

WORKLOADS = (
    "verilator",
    "kafka.final",
    "benchbase-tpcc2.no_security_features",
    "tomcat.dacapo-new",
    "benchbase-ycsb",
    "benchbase-twitter",
    "benchbase-voter",
    "benchbase-smallbank",
    "benchbase-tatp",
    "benchbase-sibench",
    "benchbase-noop",
)

PAPER_LABELS = {
    "verilator": "verilator",
    "kafka.final": "kafka",
    "benchbase-tpcc2.no_security_features": "tpcc",
    "tomcat.dacapo-new": "tomcat",
    "benchbase-ycsb": "ycsb",
    "benchbase-twitter": "twitter",
    "benchbase-voter": "voter",
    "benchbase-smallbank": "sbank",
    "benchbase-tatp": "tatp",
    "benchbase-sibench": "sib",
    "benchbase-noop": "noop",
}

PAPER_ORDER = (
    "kafka.final", "tomcat.dacapo-new", "benchbase-tpcc2.no_security_features",
    "benchbase-ycsb", "benchbase-twitter", "benchbase-voter",
    "benchbase-smallbank", "benchbase-tatp", "benchbase-sibench",
    "benchbase-noop", "verilator",
)


def paper_label(workload: str) -> str:
    return PAPER_LABELS.get(workload, workload)


def paper_workload_order(workloads: Iterable[str]) -> list[str]:
    available = set(workloads)
    return [workload for workload in PAPER_ORDER if workload in available]


def paper_rc() -> None:
    """Set the compact serif style used by the paper's plots."""
    import matplotlib
    matplotlib.rcParams.update({
        "font.family": "serif",
        "font.serif": ["Times New Roman", "DejaVu Serif"],
        "font.size": 7,
        "axes.labelsize": 8,
        "axes.titlesize": 8,
        "xtick.labelsize": 7,
        "ytick.labelsize": 7,
        "legend.fontsize": 7,
        "axes.linewidth": 0.6,
        "xtick.major.width": 0.5,
        "ytick.major.width": 0.5,
        "pdf.fonttype": 42,
        "ps.fonttype": 42,
    })

GEM5_NAMES = {
    "verilator": "verilator",
    "kafka.final": "dacapo-kafka",
    "benchbase-tpcc2.no_security_features": "benchbase-tpcc",
    "tomcat.dacapo-new": "dacapo-tomcat",
    "benchbase-ycsb": "benchbase-ycsb",
    "benchbase-twitter": "benchbase-twitter",
    "benchbase-voter": "benchbase-voter",
    "benchbase-smallbank": "benchbase-smallbank",
    "benchbase-tatp": "benchbase-tatp",
    "benchbase-sibench": "benchbase-sibench",
    "benchbase-noop": "benchbase-noop",
}


def run_configs(
    output: Path,
    configs: Iterable[str],
    *,
    pdip_assoc: int = 8,
    pdip_sets: int = 512,
    btb_entries: int = 16 * 1024,
    l2_rp: str = "lru",
    fec_monitor: bool = False,
    jobs: int = 8,
    force: bool = False,
    dry_run: bool = False,
    workloads: Iterable[str] | None = None,
    max_weight_only: bool = False,
) -> None:
    command = [
        "python3",
        str(RUNNER),
        "--root",
        str(BENCH_ROOT),
        "--gem5",
        str(GEM5_BIN),
        "--out",
        str(output),
        "--warmup",
        "10000000",
        "--measurement",
        "10000000",
        "--jobs",
        str(jobs),
    ]
    for workload in workloads or WORKLOADS:
        command += ["--workload", workload]
    for config in configs:
        command += ["--config-name", config]
    command += [
        "--pdip-table-sets",
        str(pdip_sets),
        "--pdip-table-assoc",
        str(pdip_assoc),
        "--btb-entries",
        str(btb_entries),
        "--l2-rp",
        l2_rp,
    ]
    if fec_monitor:
        command.append("--fec-monitor")
    if force:
        command.append("--force")
    else:
        # Remove stale partial/dry-run job directories while preserving
        # completed stats so an interrupted matrix can be resumed safely.
        command.append("--retry-incomplete")
    if dry_run:
        command.append("--dry-run")
    if max_weight_only:
        command.append("--max-weight-only")
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(command, check=True, cwd=GEM5_ROOT)


def stats_blocks(path: Path) -> list[dict[str, float]]:
    blocks: list[dict[str, float]] = []
    current: dict[str, float] | None = None
    for line in path.read_text(errors="replace").splitlines():
        if "Begin Simulation Statistics" in line:
            current = {}
        elif "End Simulation Statistics" in line:
            if current is not None:
                blocks.append(current)
            current = None
        elif current is not None:
            fields = line.split()
            if len(fields) >= 2:
                try:
                    current[fields[0]] = float(fields[1])
                except ValueError:
                    pass
    return blocks


def last_value(stats: dict[str, float], suffix: str) -> float | None:
    values = [value for key, value in stats.items() if key.endswith(suffix)]
    return values[-1] if values else None


def sum_values(stats: dict[str, float], suffix: str) -> float | None:
    values = [value for key, value in stats.items() if key.endswith(suffix)]
    return sum(values) if values else None


def measurement(run_dir: Path) -> dict[str, float] | None:
    stats_path = run_dir / "stats.txt"
    if not stats_path.is_file():
        return None
    blocks = stats_blocks(stats_path)
    return blocks[-1] if blocks else None


def family(workload: str) -> str:
    return "amd64-java" if workload.startswith("benchbase-") or workload in {
        "kafka.final", "tomcat.dacapo-new"
    } else "amd64"


def weights(workload: str) -> dict[int, float]:
    path = (
        BENCH_ROOT / "results" / family(workload) / "simpoint-clusters"
        / GEM5_NAMES[workload] / "results.weights"
    )
    result: dict[int, float] = {}
    if not path.is_file():
        return result
    for line in path.read_text().splitlines():
        fields = line.split()
        if len(fields) == 2:
            try:
                result[int(fields[1])] = float(fields[0])
            except ValueError:
                pass
    return result


def weighted_metric(
    root: Path,
    workload: str,
    config: str,
    suffix: str,
) -> float | None:
    total = 0.0
    weight_sum = 0.0
    run_root = root / workload / config
    for sid, weight in weights(workload).items():
        stats = measurement(run_root / f"simpoint-{sid}")
        if stats is None:
            continue
        value = sum_values(stats, suffix)
        if value is None:
            continue
        total += weight * value
        weight_sum += weight
    return total / weight_sum if weight_sum else None


def monitor_metric(
    root: Path,
    workload: str,
    config: str,
    stat_name: str,
) -> float | None:
    """Weighted stat from the monitor-only PDIP instance in each run.

    The MultiPrefetcher index is configuration-dependent (FDIP puts the
    monitor at index 1, while PDIP/EIP commonly put it at index 2), so infer
    it from config.ini instead of hard-coding an object number.
    """
    total = 0.0
    weight_sum = 0.0
    run_root = root / workload / config
    for sid, weight in weights(workload).items():
        run_dir = run_root / f"simpoint-{sid}"
        stats = measurement(run_dir)
        config_path = run_dir / "config.ini"
        if stats is None or not config_path.is_file():
            continue
        text = config_path.read_text(errors="replace")
        monitor_indices: list[int] = []
        section_index: int | None = None
        for line in text.splitlines():
            if line.startswith("[") and "prefetchers" in line:
                marker = line.split("prefetchers", 1)[1].split("]", 1)[0]
                section_index = int(marker) if marker.isdigit() else None
            elif line.startswith("["):
                section_index = None
            elif (section_index is not None and
                  line.strip() == "monitor_only=true"):
                monitor_indices.append(section_index)
        values = [
            value for key, value in stats.items()
            if any(key.endswith(f"prefetchers{index}.{stat_name}")
                   for index in monitor_indices)
        ]
        if not values:
            continue
        total += weight * sum(values)
        weight_sum += weight
    return total / weight_sum if weight_sum else None


def weighted_ipc(root: Path, workload: str, config: str) -> float | None:
    weighted_insts = 0.0
    weighted_cycles = 0.0
    weight_sum = 0.0
    run_root = root / workload / config
    for sid, weight in weights(workload).items():
        stats = measurement(run_root / f"simpoint-{sid}")
        if stats is None:
            continue
        insts = sum_values(stats, "thread_0.numInsts")
        cycles = sum_values(stats, "core.numCycles")
        if insts is None or cycles is None or cycles <= 0:
            continue
        weighted_insts += weight * insts
        weighted_cycles += weight * cycles
        weight_sum += weight
    if not weight_sum or weighted_cycles <= 0:
        return None
    return weighted_insts / weighted_cycles


def geomean(values: Iterable[float]) -> float:
    values = list(values)
    return math.exp(sum(math.log(value) for value in values) / len(values))


def write_csv(path: Path, rows: list[dict[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def write_manifest(path: Path, data: object) -> None:
    path.write_text(json.dumps(data, indent=2) + "\n")
