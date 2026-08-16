/*
 * Copyright (c) 2026
 * All rights reserved.
 */

#include "mem/cache/prefetch/udp.hh"

#include <algorithm>

#include "params/UtilityDirectedPrefetcher.hh"

namespace gem5
{
namespace prefetch
{

UtilityDirectedPrefetcher::BloomFilter::BloomFilter(unsigned bits,
                                                     unsigned hashes)
  : numBits(bits), numHashes(hashes), words((bits + 63) / 64, 0)
{
    fatal_if(bits == 0 || hashes == 0, "UDP Bloom filters must be non-empty");
}

uint64_t
UtilityDirectedPrefetcher::BloomFilter::hash(Addr key, unsigned salt) const
{
    uint64_t value = key + 0x9e3779b97f4a7c15ULL * (salt + 1);
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

bool
UtilityDirectedPrefetcher::BloomFilter::contains(Addr key) const
{
    for (unsigned hash_index = 0; hash_index < numHashes; ++hash_index) {
        const unsigned bit = hash(key, hash_index) % numBits;
        if (!(words[bit / 64] & (uint64_t(1) << (bit % 64))))
            return false;
    }
    return true;
}

void
UtilityDirectedPrefetcher::BloomFilter::insert(Addr key)
{
    for (unsigned hash_index = 0; hash_index < numHashes; ++hash_index) {
        const unsigned bit = hash(key, hash_index) % numBits;
        words[bit / 64] |= uint64_t(1) << (bit % 64);
    }
}

void
UtilityDirectedPrefetcher::BloomFilter::clear()
{
    std::fill(words.begin(), words.end(), 0);
}

UtilityDirectedPrefetcher::UtilityDirectedPrefetcher(const Params &p)
  : FetchDirectedPrefetcher(p), stats(this),
    confidenceThreshold(p.confidence_threshold),
    seniorityEntries(p.seniority_entries), bloom1Entries(p.bloom1_entries),
    bloom2Entries(p.bloom2_entries), bloom4Entries(p.bloom4_entries),
    bloomClearUnusefulRatio(p.bloom_clear_unuseful_ratio),
    bloomClearPeriod(cyclesToTicks(p.bloom_clear_period)),
    bloom1(p.bloom1_bits, p.bloom_hashes),
    bloom2(p.bloom2_bits, p.bloom_hashes),
    bloom4(p.bloom4_bits, p.bloom_hashes), utilityPeriodStart(curTick())
{
    fatal_if(seniorityEntries == 0, "UDP seniority FTQ must be non-empty");
    fatal_if(bloomClearUnusefulRatio < 0.0 || bloomClearUnusefulRatio > 1.0,
             "UDP Bloom clear ratio must be in [0, 1]");
}

void
UtilityDirectedPrefetcher::regProbeListeners()
{
    FetchDirectedPrefetcher::regProbeListeners();
    if (!cpu)
        return;

    using FTListener = ProbeListenerArgFunc<o3::FetchTargetPtr>;
    using InstListener = ProbeListenerArgFunc<o3::DynInstPtr>;
    udpListeners.push_back(cpu->getProbeManager()->connect<FTListener>(
        "FTQRemove", [this](const auto &ft) { notifyFTQRemove(ft); }));
    udpListeners.push_back(cpu->getProbeManager()->connect<FTListener>(
        "FTQSquash", [this](const auto &ft) { notifyFTQSquash(ft); }));
    udpListeners.push_back(cpu->getProbeManager()->connect<InstListener>(
        "Mispredict", [this](const auto &inst) { notifyMispredict(inst); }));
    udpListeners.push_back(cpu->getProbeManager()->connect<InstListener>(
        "Commit", [this](const auto &inst) { notifyCommit(inst); }));
}

void
UtilityDirectedPrefetcher::notifyFTQInsert(const o3::FetchTargetPtr &ft)
{
    const ThreadID tid = ft->getTid();
    offPathTargets[ft->ftNum()] = confidenceScore[tid] > confidenceThreshold;
    FetchDirectedPrefetcher::notifyFTQInsert(ft);

    if (!ft->isExitBranch(ft->endAddress()) || !ft->bpuHistory)
        return;

    using Confidence = branch_prediction::ConditionalPredictor::PredictionConfidence;
    switch (ft->bpuHistory->confidence) {
      case Confidence::Low:
        confidenceScore[tid] += 2;
        break;
      case Confidence::Medium:
        confidenceScore[tid] += 1;
        break;
      case Confidence::High:
        break;
    }

    // A taken prediction without a BTB target is explicitly treated as an
    // off-path transition by the UDP paper and the Scarab implementation.
    if (ft->bpuHistory->predTaken && !ft->bpuHistory->btbHit)
        confidenceScore[tid] = confidenceThreshold + 1;
}

bool
UtilityDirectedPrefetcher::usefulSetContains(Addr line) const
{
    return bloom1.contains(line) || bloom2.contains(line >> 1) ||
           bloom4.contains(line >> 2);
}

void
UtilityDirectedPrefetcher::addSeniorityCandidate(Addr line, ThreadID tid,
                                                  o3::FTSeqNum ftn)
{
    if (seniorityFtq.size() == seniorityEntries) {
        seniorityFtq.pop_front();
        stats.seniorityEvictions++;
    }
    seniorityFtq.push_back({line, ftn, tid, curTick()});
    stats.seniorityInsertions++;
}

bool
UtilityDirectedPrefetcher::allowPrefetch(Addr addr, ThreadID tid,
                                          o3::FTSeqNum ftn)
{
    // Scarab's Bloom filters index cache lines, rather than byte addresses.
    const Addr line = blockIndex(addr);
    stats.candidates++;
    const auto found = offPathTargets.find(ftn);
    const bool off_path = found != offPathTargets.end() && found->second;
    if (!off_path) {
        stats.onPathCandidates++;
        return true;
    }

    stats.offPathCandidates++;
    if (usefulSetContains(line)) {
        stats.usefulSetHits++;
        return true;
    }

    stats.usefulSetMisses++;
    stats.filteredCandidates++;
    addSeniorityCandidate(line, tid, ftn);
    return false;
}

void
UtilityDirectedPrefetcher::notifyPrefetchIssued(Addr, Addr)
{
    ++newPrefetches;
}

void
UtilityDirectedPrefetcher::notify(const CacheAccessProbeArg &,
                                  const PrefetchInfo &)
{
    // Paper UDP learns only when a correct-path instruction retires, not on
    // speculative L1I demand accesses.
}

void
UtilityDirectedPrefetcher::notifyEvict(const CacheDataUpdateProbeArg &info)
{
    if (!info.hwPrefetched || !info.newData.empty() ||
        info.requestorID != requestorId) {
        return;
    }
    ++unusefulPrefetches;
    stats.unusefulPrefetches++;
}

void
UtilityDirectedPrefetcher::learnRetired(Addr pc, ThreadID tid)
{
    const Addr line = blockIndex(pc);
    for (auto it = seniorityFtq.begin(); it != seniorityFtq.end(); ++it) {
        if (it->line != line || it->tid != tid)
            continue;
        learnUseful(line);
        seniorityFtq.erase(it);
        stats.seniorityHits++;
        return;
    }
}

void
UtilityDirectedPrefetcher::learnUseful(Addr line)
{
    if (!recentUseful.empty() && line != recentUseful.back() + 1)
        flushUsefulRun();
    recentUseful.push_back(line);
    if (recentUseful.size() == 8)
        flushUsefulRun();
}

void
UtilityDirectedPrefetcher::flushUsefulRun()
{
    if (recentUseful.empty())
        return;

    Addr line = recentUseful.front();
    unsigned remaining = recentUseful.size();
    while (remaining) {
        if (!(line & 3) && remaining >= 4) {
            insertUsefulRegion(line, 4);
            line += 4;
            remaining -= 4;
        } else if (!(line & 1) && remaining >= 2) {
            insertUsefulRegion(line, 2);
            line += 2;
            remaining -= 2;
        } else {
            insertUsefulRegion(line, 1);
            ++line;
            --remaining;
        }
    }
    recentUseful.clear();
}

void
UtilityDirectedPrefetcher::maybeClearBloom(unsigned filter)
{
    const unsigned capacity = filter == 0 ? bloom1Entries :
                              filter == 1 ? bloom2Entries : bloom4Entries;
    if (bloomInsertions[filter] < capacity || newPrefetches == 0 ||
        double(unusefulPrefetches) / newPrefetches <= bloomClearUnusefulRatio) {
        return;
    }
    if (filter == 0)
        bloom1.clear();
    else if (filter == 1)
        bloom2.clear();
    else
        bloom4.clear();
    bloomInsertions[filter] = 0;
    stats.bloomClears++;
}

void
UtilityDirectedPrefetcher::insertUsefulRegion(Addr line, unsigned lines)
{
    const unsigned filter = lines == 1 ? 0 : lines == 2 ? 1 : 2;

    // Match Scarab's hierarchical Bloom insertion policy: a region already
    // represented by a larger super-line does not consume a smaller entry.
    if (filter == 0 &&
        (bloom2.contains(line >> 1) || bloom4.contains(line >> 2))) {
        return;
    }
    if (filter == 1 &&
        (bloom4.contains(line >> 2) || bloom2.contains(line >> 1))) {
        return;
    }

    maybeClearBloom(filter);
    if (filter == 0)
        bloom1.insert(line);
    else if (filter == 1)
        bloom2.insert(line >> 1);
    else
        bloom4.insert(line >> 2);
    ++bloomInsertions[filter];
    stats.usefulInsertions++;
}

void
UtilityDirectedPrefetcher::resetConfidence(ThreadID tid)
{
    confidenceScore[tid] = 0;
    stats.confidenceResets++;
}

void
UtilityDirectedPrefetcher::notifyFTQRemove(const o3::FetchTargetPtr &ft)
{
    offPathTargets.erase(ft->ftNum());
}

void
UtilityDirectedPrefetcher::notifyFTQSquash(const o3::FetchTargetPtr &ft)
{
    const auto ftn = ft->ftNum();
    offPathTargets.erase(ftn);
    seniorityFtq.erase(std::remove_if(seniorityFtq.begin(), seniorityFtq.end(),
        [ftn](const auto &candidate) { return candidate.ftn == ftn; }),
        seniorityFtq.end());
    resetConfidence(ft->getTid());
}

void
UtilityDirectedPrefetcher::notifyMispredict(const o3::DynInstPtr &inst)
{
    if (inst)
        resetConfidence(inst->threadNumber);
}

void
UtilityDirectedPrefetcher::notifyCommit(const o3::DynInstPtr &inst)
{
    if (inst)
        learnRetired(inst->pcState().instAddr(), inst->threadNumber);
    if (bloomClearPeriod && curTick() - utilityPeriodStart >= bloomClearPeriod) {
        utilityPeriodStart = curTick();
        newPrefetches = 0;
        unusefulPrefetches = 0;
    }
}

UtilityDirectedPrefetcher::Stats::Stats(statistics::Group *parent)
  : statistics::Group(parent),
    ADD_STAT(candidates, statistics::units::Count::get(), "UDP candidates"),
    ADD_STAT(onPathCandidates, statistics::units::Count::get(), "UDP on-path candidates"),
    ADD_STAT(offPathCandidates, statistics::units::Count::get(), "UDP confidence-off-path candidates"),
    ADD_STAT(usefulSetHits, statistics::units::Count::get(), "UDP useful-set hits"),
    ADD_STAT(usefulSetMisses, statistics::units::Count::get(), "UDP useful-set misses"),
    ADD_STAT(filteredCandidates, statistics::units::Count::get(), "UDP suppressed off-path candidates"),
    ADD_STAT(seniorityInsertions, statistics::units::Count::get(), "UDP Seniority-FTQ insertions"),
    ADD_STAT(seniorityHits, statistics::units::Count::get(), "UDP Seniority-FTQ retired hits"),
    ADD_STAT(seniorityEvictions, statistics::units::Count::get(), "UDP Seniority-FTQ capacity evictions"),
    ADD_STAT(usefulInsertions, statistics::units::Count::get(), "UDP useful-set insertions"),
    ADD_STAT(bloomClears, statistics::units::Count::get(), "UDP Bloom filter clears"),
    ADD_STAT(unusefulPrefetches, statistics::units::Count::get(), "UDP unused prefetched blocks"),
    ADD_STAT(confidenceResets, statistics::units::Count::get(), "UDP recovery confidence resets")
{
}

} // namespace prefetch
} // namespace gem5
