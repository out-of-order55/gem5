/*
 * Copyright (c) 2026
 * All rights reserved.
 *
 * UFTQ: Utility-driven Fetch Target Queue sizing, ISCA 2024.
 */

#ifndef __MEM_CACHE_PREFETCH_UFTQ_HH__
#define __MEM_CACHE_PREFETCH_UFTQ_HH__

#include <unordered_set>

#include "cpu/o3/cpu.hh"
#include "mem/cache/prefetch/udp.hh"

namespace gem5
{

struct UtilityFetchTargetQueuePrefetcherParams;
struct UtilityDirectedFetchTargetQueuePrefetcherParams;

namespace prefetch
{

/**
 * Paper UFTQ controller on top of FDIP. It keeps FDIP's candidate stream and
 * changes only the effective depth at which the O3 FTQ blocks the BAC.
 */
class UtilityFetchTargetQueuePrefetcher : public FetchDirectedPrefetcher
{
  public:
    using Params = UtilityFetchTargetQueuePrefetcherParams;

    UtilityFetchTargetQueuePrefetcher(const Params &p);

    void regProbeListeners() override;
    void notify(const CacheAccessProbeArg &acc,
                const PrefetchInfo &pfi) override;
    void notifyFill(const CacheAccessProbeArg &acc) override;
    void prefetchUnused() override;

  protected:
    void notifyPrefetchIssued(Addr candidate_addr,
                              Addr physical_addr) override;

  private:
    enum class Policy { Aur, Atr, AurAtr };
    enum class SearchStage { Aur, Atr };

    struct Stats : public statistics::Group {
        Stats(statistics::Group *parent);
        statistics::Scalar windows;
        statistics::Scalar depthIncreases;
        statistics::Scalar depthDecreases;
        statistics::Scalar aurConvergences;
        statistics::Scalar atrConvergences;
        statistics::Scalar regressionUpdates;
        statistics::Scalar usefulPrefetches;
        statistics::Scalar unusefulPrefetches;
        statistics::Scalar timelyPrefetches;
        statistics::Scalar untimelyPrefetches;
    } uftqStats;

    const Policy policy;
    const unsigned initialDepth;
    const unsigned minDepth;
    const unsigned maxDepth;
    const unsigned depthStep;
    const unsigned measurementPeriod;
    const double aurTarget;
    const double atrTarget;

    o3::CPU *o3Cpu;
    SearchStage searchStage = SearchStage::Aur;
    int previousDirection = 0;
    unsigned qdAur = 0;
    unsigned qdAtr = 0;

    uint64_t issuedInWindow = 0;
    uint64_t usefulInWindow = 0;
    uint64_t unusefulInWindow = 0;
    uint64_t timelyInWindow = 0;
    uint64_t untimelyInWindow = 0;

    // Lines whose FDIP requests have left the PFQ and may still be in MSHRs.
    std::unordered_set<Addr> issuedLines;

    static Policy parsePolicy(const std::string &name);
    unsigned currentDepth() const;
    void setDepth(unsigned depth);
    bool seekRatio(double ratio, double target, unsigned &depth);
    void evaluateWindow();
    void resetWindow();
};

/**
 * UDP and UFTQ as one FDIP stream. UDP filters candidates and learns the
 * useful set, while UFTQ adapts the effective FTQ depth from that same
 * stream's feedback. This deliberately is not a MultiPrefetcher: two FDIP
 * instances would each walk the FTQ and issue duplicate requests.
 */
class UtilityDirectedFetchTargetQueuePrefetcher :
    public UtilityDirectedPrefetcher
{
  public:
    using Params = UtilityDirectedFetchTargetQueuePrefetcherParams;

    UtilityDirectedFetchTargetQueuePrefetcher(const Params &p);

    void regProbeListeners() override;
    void notify(const CacheAccessProbeArg &acc,
                const PrefetchInfo &pfi) override;
    void notifyFill(const CacheAccessProbeArg &acc) override;
    void prefetchUnused() override;

  protected:
    void notifyPrefetchIssued(Addr candidate_addr,
                              Addr physical_addr) override;

  private:
    enum class Policy { Aur, Atr, AurAtr };
    enum class SearchStage { Aur, Atr };

    struct Stats : public statistics::Group {
        Stats(statistics::Group *parent);
        statistics::Scalar windows;
        statistics::Scalar depthIncreases;
        statistics::Scalar depthDecreases;
        statistics::Scalar aurConvergences;
        statistics::Scalar atrConvergences;
        statistics::Scalar regressionUpdates;
        statistics::Scalar usefulPrefetches;
        statistics::Scalar uftqUnusefulPrefetches;
        statistics::Scalar timelyPrefetches;
        statistics::Scalar untimelyPrefetches;
    } uftqStats;

    const Policy policy;
    const unsigned initialDepth;
    const unsigned minDepth;
    const unsigned maxDepth;
    const unsigned depthStep;
    const unsigned measurementPeriod;
    const double aurTarget;
    const double atrTarget;

    o3::CPU *o3Cpu;
    SearchStage searchStage = SearchStage::Aur;
    int previousDirection = 0;
    unsigned qdAur = 0;
    unsigned qdAtr = 0;

    uint64_t issuedInWindow = 0;
    uint64_t usefulInWindow = 0;
    uint64_t unusefulInWindow = 0;
    uint64_t timelyInWindow = 0;
    uint64_t untimelyInWindow = 0;
    std::unordered_set<Addr> issuedLines;

    static Policy parsePolicy(const std::string &name);
    unsigned currentDepth() const;
    void setDepth(unsigned depth);
    bool seekRatio(double ratio, double target, unsigned &depth);
    void evaluateWindow();
    void resetWindow();
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_UFTQ_HH__
