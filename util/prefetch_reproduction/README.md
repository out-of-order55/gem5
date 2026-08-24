# UDP and D-JOLT Reproduction

This directory documents the gem5 implementation and the Tomcat SimPoint
experiment. The implementations are based on the public Scarab v2.0 source at
commit `ada6624503f286721dfe1d651d122e8efd4375af`.

| Mechanism | Paper/open-source design | gem5 implementation |
| --- | --- | --- |
| FDIP | FTQ candidates, virtual-address translation, squashable PFQ | `src/mem/cache/prefetch/fdp.*` |
| UFTQ | Runtime FTQ-depth controller using AUR, ATR, or their combined model | `src/mem/cache/prefetch/uftq.*` |
| UDP | Confidence-gated FDIP with 1/2/4-line Bloom useful-set and Seniority-FTQ | `src/mem/cache/prefetch/udp.*` |
| D-JOLT | FIFO_RETCNT; short `(histlen=4,distance=4)` and long `(7,15)` predictors; shared extra miss table; cache-line based fallback stream prefetcher | `src/mem/cache/prefetch/d_jolt.*` |

`UtilityDirectedPrefetcher` is a subclass of
`FetchDirectedPrefetcher`. It observes and filters the exact FDIP FTQ
candidates; the FDIP implementation records each issued request's virtual
candidate and physical cache-line address so L1I useful/unused feedback is
attributed to the correct utility-cache entry.
UDP follows the paper's 8 KiB Bloom useful-set design. High-confidence FDIP
candidates issue normally; confidence-identified off-path candidates issue
only after a useful-set hit.

`DistantJoltPrefetcher` listens to the O3 `Fetch` probe, which includes
wrong-path fetched control instructions, and to L1I hit/miss probes. It is
always configured beside FDIP, not in place of it.

## Design correspondence

### UFTQ

`UtilityFetchTargetQueuePrefetcher` retains FDIP's prefetch stream and
controls only the O3 FTQ's effective capacity. The configured
`numFTQEntries` remains the physical per-thread capacity; UFTQ can reduce the
runtime capacity without dropping resident targets, so the BAC stalls until
the queue naturally drains below the new limit. The paper's 1,000-prefetch
feedback interval, initial depth 32, and AUR+ATR polynomial are defaults.

The controller measures useful/(useful+unused) L1I prefetches as AUR. It
counts a prefetched L1I demand hit as timely and a demand that finds its FDIP
request still in the miss queue as late. The standalone `aur` and `atr`
policies tune their respective targets directly. The default `aur_atr` policy
first finds `QD_AUR`, then `QD_ATR`, and applies the paper's regression:

```
FTQ = -0.34 QD_AUR + 0.64 QD_ATR + 0.008 QD_AUR^2
      + 0.01 QD_ATR^2 - 0.008 QD_AUR QD_ATR
```

`max_depth` must not exceed the O3 CPU's physical `numFTQEntries`; configure
that value to at least 64 for the default UFTQ settings.

### UDP

The paper's primary UDP configuration and the Scarab v2.0 artifact use a
confidence-gated Bloom useful-set. The gem5 implementation applies that policy
at the equivalent FDIP candidate point:

| Scarab/UDP behavior | gem5 counterpart |
| --- | --- |
| FDIP produces a candidate cache line from its fetch-target queue. | FetchDirectedPrefetcher::notifyFTQInsert() calls the derived allowPrefetch() hook. |
| Low/medium/high branch confidences add 2/1/0 to a score; an above-threshold score identifies the off path (Scarab threshold 15). | TAGE-SC-L confidence is propagated through BPredUnit and consumed per FTQ target. Taken predictions that miss the BTB also force the following target off path. |
| On-path candidates issue unconditionally; off-path candidates require a useful-set hit. | allowPrefetch() queries the useful-set only for confidence-off-path FTQ targets. |
| Three Bloom filters store 1-line (16K bits), 2-line (1K bits), and 4-line (1K bits) regions using six hashes. | BloomFilter plus the recent eight-candidate stream compressor. |
| Learn a useful candidate only when a correct-path instruction retires from a matching Seniority-FTQ block; remove on a pipeline flush. | Commit and FTQSquash probes maintain the bounded Seniority-FTQ. |
| Clear a full filter when unused-prefetch ratio exceeds the paper threshold. | Per-filter insertion counters and bloom_clear_unuseful_ratio=0.75. |

The useful-set and Seniority-FTQ use the cache-line index used by FDIP's
candidate stream. L1I eviction feedback is used only for the Bloom-filter unused-ratio
clearing policy; it does not train individual candidates.

### D-JOLT

The D-JOLT implementation follows Scarab's public 8 KiB comparison setup:

| Scarab/D-JOLT behavior | gem5 counterpart |
| --- | --- |
| Update FIFO_RETCNT on conditional, indirect, repeat, and return controls at fetch time. | O3 Fetch probe in onFetch() filters isCondCtrl(), isIndirectCtrl(), and isReturn(). |
| Short (history=4, distance=4) and long (7,15) rolling signatures. | SignatureGenerator plus SignatureQueue instances. |
| 32-set/4-way short table, 64-set/4-way long table, shared 256-set/4-way extra table; two 8-line vectors per entry. | MissTable instances and MissInfo. |
| Fixed 31-entry upper-bit compression table. | UpperBitTable. |
| Learn only L1I demand misses; use the delayed signature; keep primary and extra-table LRU state current. | L1I probes call accessCache() and learn(). |
| 16-entry training and 16-entry monitoring stream fallback, with 2-line window/distance and degree 2. | StreamPrefetcher, using cache-line addresses exactly as Scarab does. |

--d-jolt forces the FDIP decoupled front end and creates one
MultiPrefetcher containing FetchDirectedPrefetcher and
DistantJoltPrefetcher. Thus D-JOLT augments the FDIP request stream instead
of replacing it; the generated config.ini is checked for this pair.

Run the full experiment with:

```bash
bash /data1/GB/gem5-svr-bench/scripts/run-tomcat-prefetchers.sh
```

The script restores
`results/amd64-java/simpoint-checkpoints/dacapo-tomcat/cpt.SimPoint0`, warms
100M instructions, measures 100M instructions, and runs FDIP, PDIP, EIP,
UDP, and FDIP+D-JOLT. It writes per-run logs and `stats.txt` files under
`results/amd64-java/prefetch-reproduction/dacapo-tomcat-simpoint0`, followed
by `results.csv`, `results.json`, `ipc.png`, and `speedup.png`.
It also writes `validation.json`, after checking that every log reached the
normal interval-complete marker without a fatal/panic, every run has warmup
and measurement statistic blocks, the final block has exactly 100M
instructions, and the recorded L1I prefetcher components match the requested
configuration.
The structured results record the exact measured instruction count, cycles,
IPC, aggregate L1I prefetch requests/useful requests, prefetch accuracy,
coverage, prefetches per kilo-instruction, estimated 64-byte-line traffic,
request hits in cache/MSHR/write-buffer (late prefetches), and L1I demand
hit/miss counts. The
`MultiPrefetcher` aggregate is used for request traffic, while useful/unused
feedback is summed only across its immediate children; this avoids both the
missing top-level feedback in gem5 and double-counting an FDIP+D-JOLT run.

## PDIP figure experiments

The four entry points below are self-contained wrappers located in the gem5
tree. They call the existing full-system workload runner, use the workloads
and SimPoint checkpoints currently available on this machine, and write every
run and generated graph below `/data1/GB/gem5/results/prefetch_reproduction/`.
The scripts use a 10M-instruction warmup and a 10M-instruction measurement by
default. Missing paper workloads are recorded in each `manifest.json` and are
skipped by the current runner.

```bash
cd /data1/GB/gem5

# Figure 12: FEC-stall reduction for PDIP(44) and EIP(46) vs FDIP
./util/prefetch_reproduction/run_fig12.sh

# Figure 13: PDIP table-associativity sensitivity (512 sets, 2/4/8/16 ways)
./util/prefetch_reproduction/run_fig13.sh

# Figure 14: IPC speedup at six BTB sizes (4K through 128K entries)
./util/prefetch_reproduction/run_fig14.sh

# Figure 16: issued prefetch-trigger distribution
./util/prefetch_reproduction/run_fig16.sh
```

Each script also accepts `--dry-run` to print and record all gem5 commands
without running simulations, `--force` to rerun existing jobs, and
`--skip-run` to regenerate CSV/PDF/PNG files from completed results. The
optional `--workload NAME` (repeatable), `--max-weight-only`, and `--jobs N`
options are useful for a quick subset or highest-weight SimPoint check before
launching the full matrix.

Incomplete job directories are automatically retried; completed jobs with a
valid `stats.txt` are reused.

To only generate the four graphs from existing simulation results, without
starting gem5, run:

```bash
./util/prefetch_reproduction/plot_all.sh
```

Use `--figure fig12` (or `fig13`, `fig14`, `fig16`) for one graph. Add
`--continue-on-error` to generate the other graphs when one result directory
is incomplete.
generated files are:

* `fig12/fec-stall-reduction.{csv,png,pdf}`
* `fig13/pdip-table-sensitivity.{csv,png,pdf}`
* `fig14/btb-sensitivity.{csv,png,pdf}`
* `fig16/trigger-distribution.{csv,png,pdf}`

Per-workload `stats.txt`, `gem5.log`, `manifest.json`, and `run-summary.json`
remain in the corresponding figure directory for auditability. Figure 12
uses the monitor-only PDIP counter to count decode/FEC stalls without adding
prefetch requests. Figure 16 reports the two issued-target counters recorded
by the active PDIP instance.
