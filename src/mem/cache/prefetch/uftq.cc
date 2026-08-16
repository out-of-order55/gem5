/*
 * Copyright (c) 2026
 * All rights reserved.
 */

#include "mem/cache/prefetch/uftq.hh"

#include <algorithm>
#include <cmath>

#include "params/UtilityFetchTargetQueuePrefetcher.hh"

namespace gem5
{
namespace prefetch
{

UtilityFetchTargetQueuePrefetcher::Policy
UtilityFetchTargetQueuePrefetcher::parsePolicy(const std::string &name)
{
    if (name == "aur")
        return Policy::Aur;
    if (name == "atr")
        return Policy::Atr;
    if (name == "aur_atr")
        return Policy::AurAtr;
    fatal("UFTQ policy must be one of: aur, atr, aur_atr");
}

UtilityFetchTargetQueuePrefetcher::UtilityFetchTargetQueuePrefetcher(
    const Params &p)
  : FetchDirectedPrefetcher(p), uftqStats(this), policy(parsePolicy(p.policy)),
    initialDepth(p.initial_depth), minDepth(p.min_depth), maxDepth(p.max_depth),
    depthStep(p.depth_step), measurementPeriod(p.measurement_period),
    aurTarget(p.aur_target), atrTarget(p.atr_target),
    o3Cpu(dynamic_cast<o3::CPU *>(cpu))
{
    fatal_if(!o3Cpu, "UFTQ requires an O3 CPU with a decoupled frontend");
    fatal_if(minDepth == 0 || minDepth > initialDepth ||
                 initialDepth > maxDepth,
             "UFTQ depths must satisfy 1 <= min_depth <= initial_depth "
             "<= max_depth");
    fatal_if(maxDepth > o3Cpu->ftqPhysicalCapacity(),
             "UFTQ max_depth %u exceeds the FTQ physical capacity %u",
             maxDepth, o3Cpu->ftqPhysicalCapacity());
    fatal_if(depthStep == 0 || measurementPeriod == 0,
             "UFTQ depth_step and measurement_period must be non-zero");
    fatal_if(aurTarget < 0.0 || aurTarget > 1.0 ||
                 atrTarget < 0.0 || atrTarget > 1.0,
             "UFTQ AUR and ATR targets must be in [0, 1]");
}

void
UtilityFetchTargetQueuePrefetcher::regProbeListeners()
{
    FetchDirectedPrefetcher::regProbeListeners();
    setDepth(initialDepth);
}

void
UtilityFetchTargetQueuePrefetcher::notify(
    const CacheAccessProbeArg &acc, const PrefetchInfo &pfi)
{
    if (!acc.pkt->isDemand() || !acc.pkt->req->isInstFetch())
        return;

    const Addr line = blockAddress(pfi.getPaddr());
    const bool prefetched = acc.cache.hasBeenPrefetched(
        pfi.getPaddr(), pfi.isSecure(), requestorId);

    if (prefetched) {
        ++usefulInWindow;
        ++timelyInWindow;
        ++uftqStats.usefulPrefetches;
        ++uftqStats.timelyPrefetches;
        issuedLines.erase(line);
    } else if (pfi.isCacheMiss() &&
               acc.cache.inMissQueue(pfi.getPaddr(), pfi.isSecure()) &&
               issuedLines.erase(line)) {
        // The demand found the FDIP request before it completed its L1I fill.
        ++usefulInWindow;
        ++untimelyInWindow;
        ++uftqStats.usefulPrefetches;
        ++uftqStats.untimelyPrefetches;
    }
}

void
UtilityFetchTargetQueuePrefetcher::notifyFill(const CacheAccessProbeArg &acc)
{
    issuedLines.erase(blockAddress(acc.pkt->getAddr()));
}

void
UtilityFetchTargetQueuePrefetcher::prefetchUnused()
{
    FetchDirectedPrefetcher::prefetchUnused();
    ++unusefulInWindow;
    ++uftqStats.unusefulPrefetches;
}

void
UtilityFetchTargetQueuePrefetcher::notifyPrefetchIssued(Addr,
                                                          Addr physical_addr)
{
    issuedLines.insert(blockAddress(physical_addr));
    ++issuedInWindow;
    if (issuedInWindow == measurementPeriod)
        evaluateWindow();
}

unsigned
UtilityFetchTargetQueuePrefetcher::currentDepth() const
{
    return o3Cpu->ftqEffectiveCapacity();
}

void
UtilityFetchTargetQueuePrefetcher::setDepth(unsigned depth)
{
    depth = std::clamp(depth, minDepth, maxDepth);
    const unsigned old_depth = currentDepth();
    if (depth == old_depth)
        return;

    o3Cpu->setFTQEffectiveEntries(depth);
    if (depth > old_depth)
        ++uftqStats.depthIncreases;
    else
        ++uftqStats.depthDecreases;
}

bool
UtilityFetchTargetQueuePrefetcher::seekRatio(double ratio, double target,
                                              unsigned &depth)
{
    const int direction = ratio > target ? 1 : ratio < target ? -1 : 0;
    const unsigned old_depth = currentDepth();

    // A target is found once the feedback crosses it, exactly reaches it, or
    // cannot move farther within the configured physical range.
    if (direction == 0 ||
        (previousDirection && direction != previousDirection) ||
        (direction > 0 && old_depth == maxDepth) ||
        (direction < 0 && old_depth == minDepth)) {
        depth = old_depth;
        previousDirection = 0;
        return true;
    }

    previousDirection = direction;
    const unsigned next_depth = direction > 0 ?
        std::min(maxDepth, old_depth + depthStep) :
        old_depth > depthStep ? std::max(minDepth, old_depth - depthStep) :
                                  minDepth;
    setDepth(next_depth);
    return false;
}

void
UtilityFetchTargetQueuePrefetcher::evaluateWindow()
{
    ++uftqStats.windows;
    const uint64_t utility_samples = usefulInWindow + unusefulInWindow;
    const uint64_t timeliness_samples = timelyInWindow + untimelyInWindow;

    if (!utility_samples && !timeliness_samples) {
        resetWindow();
        return;
    }

    const double utility = utility_samples ?
        double(usefulInWindow) / utility_samples : aurTarget;
    const double timeliness = timeliness_samples ?
        double(timelyInWindow) / timeliness_samples : atrTarget;

    switch (policy) {
      case Policy::Aur:
        seekRatio(utility, aurTarget, qdAur);
        break;
      case Policy::Atr:
        seekRatio(timeliness, atrTarget, qdAtr);
        break;
      case Policy::AurAtr:
        if (searchStage == SearchStage::Aur) {
            if (seekRatio(utility, aurTarget, qdAur)) {
                ++uftqStats.aurConvergences;
                searchStage = SearchStage::Atr;
            }
        } else if (seekRatio(timeliness, atrTarget, qdAtr)) {
            ++uftqStats.atrConvergences;
            const double qd = -0.34 * qdAur + 0.64 * qdAtr +
                0.008 * qdAur * qdAur + 0.01 * qdAtr * qdAtr -
                0.008 * qdAur * qdAtr;
            const unsigned regressed_depth = qd > 0.0 ?
                static_cast<unsigned>(std::lround(qd)) : 0;
            setDepth(regressed_depth);
            ++uftqStats.regressionUpdates;
            searchStage = SearchStage::Aur;
        }
    }

    resetWindow();
}

void
UtilityFetchTargetQueuePrefetcher::resetWindow()
{
    issuedInWindow = 0;
    usefulInWindow = 0;
    unusefulInWindow = 0;
    timelyInWindow = 0;
    untimelyInWindow = 0;
}

UtilityFetchTargetQueuePrefetcher::Stats::Stats(statistics::Group *parent)
  : statistics::Group(parent),
    ADD_STAT(windows, statistics::units::Count::get(),
             "UFTQ completed feedback windows"),
    ADD_STAT(depthIncreases, statistics::units::Count::get(),
             "UFTQ effective FTQ depth increases"),
    ADD_STAT(depthDecreases, statistics::units::Count::get(),
             "UFTQ effective FTQ depth decreases"),
    ADD_STAT(aurConvergences, statistics::units::Count::get(),
             "UFTQ AUR search convergences"),
    ADD_STAT(atrConvergences, statistics::units::Count::get(),
             "UFTQ ATR search convergences"),
    ADD_STAT(regressionUpdates, statistics::units::Count::get(),
             "UFTQ AUR+ATR regression depth updates"),
    ADD_STAT(usefulPrefetches, statistics::units::Count::get(),
             "UFTQ useful L1I prefetches"),
    ADD_STAT(unusefulPrefetches, statistics::units::Count::get(),
             "UFTQ unused L1I prefetches"),
    ADD_STAT(timelyPrefetches, statistics::units::Count::get(),
             "UFTQ timely L1I prefetches"),
    ADD_STAT(untimelyPrefetches, statistics::units::Count::get(),
             "UFTQ late L1I prefetches")
{
}

} // namespace prefetch
} // namespace gem5
