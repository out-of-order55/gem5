/*
 * Copyright (c) 2026
 * All rights reserved.
 *
 * UDP: Utility-Driven Fetch Directed Instruction Prefetching, ISCA 2024.
 */

#ifndef __MEM_CACHE_PREFETCH_UDP_HH__
#define __MEM_CACHE_PREFETCH_UDP_HH__

#include <array>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

#include "cpu/o3/dyn_inst.hh"
#include "mem/cache/prefetch/fdp.hh"

namespace gem5
{

struct UtilityDirectedPrefetcherParams;

namespace prefetch
{

/**
 * Paper UDP on top of FDIP.  High-confidence paths retain FDIP's original
 * behavior.  Candidates on a confidence-identified off path must hit the
 * learned useful-set, represented by three bounded Bloom filters for one,
 * two, and four cache-line regions.
 */
class UtilityDirectedPrefetcher : public FetchDirectedPrefetcher
{
  public:
    using Params = UtilityDirectedPrefetcherParams;

    UtilityDirectedPrefetcher(const Params &p);

    void regProbeListeners() override;
    void notify(const CacheAccessProbeArg &acc,
                const PrefetchInfo &pfi) override;
    void notifyEvict(const CacheDataUpdateProbeArg &info) override;

  protected:
    void notifyFTQInsert(const o3::FetchTargetPtr &ft) override;
    bool allowPrefetch(Addr addr, ThreadID tid, o3::FTSeqNum ftn) override;
    void notifyPrefetchIssued(Addr candidate_addr, Addr physical_addr) override;

  private:
    class BloomFilter
    {
      public:
        BloomFilter() = default;
        BloomFilter(unsigned bits, unsigned hashes);

        bool contains(Addr key) const;
        void insert(Addr key);
        void clear();

      private:
        uint64_t hash(Addr key, unsigned salt) const;

        unsigned numBits = 0;
        unsigned numHashes = 0;
        std::vector<uint64_t> words;
    };

    struct SeniorityCandidate {
        Addr line = 0;
        o3::FTSeqNum ftn = 0;
        ThreadID tid = InvalidThreadID;
        Tick inserted = 0;
    };

    struct Stats : public statistics::Group {
        Stats(statistics::Group *parent);
        statistics::Scalar candidates;
        statistics::Scalar onPathCandidates;
        statistics::Scalar offPathCandidates;
        statistics::Scalar usefulSetHits;
        statistics::Scalar usefulSetMisses;
        statistics::Scalar filteredCandidates;
        statistics::Scalar seniorityInsertions;
        statistics::Scalar seniorityHits;
        statistics::Scalar seniorityEvictions;
        statistics::Scalar usefulInsertions;
        statistics::Scalar bloomClears;
        statistics::Scalar unusefulPrefetches;
        statistics::Scalar confidenceResets;
    } stats;

    const unsigned confidenceThreshold;
    const unsigned seniorityEntries;
    const unsigned bloom1Entries;
    const unsigned bloom2Entries;
    const unsigned bloom4Entries;
    const double bloomClearUnusefulRatio;
    const Tick bloomClearPeriod;

    BloomFilter bloom1;
    BloomFilter bloom2;
    BloomFilter bloom4;
    std::array<unsigned, o3::MaxThreads> confidenceScore{};
    std::unordered_map<o3::FTSeqNum, bool> offPathTargets;
    std::deque<SeniorityCandidate> seniorityFtq;
    std::deque<Addr> recentUseful;
    std::array<unsigned, 3> bloomInsertions{};
    Tick utilityPeriodStart = 0;
    uint64_t newPrefetches = 0;
    uint64_t unusefulPrefetches = 0;
    std::vector<ProbeListenerPtr<>> udpListeners;

    bool usefulSetContains(Addr line) const;
    void addSeniorityCandidate(Addr line, ThreadID tid, o3::FTSeqNum ftn);
    void learnRetired(Addr pc, ThreadID tid);
    void learnUseful(Addr line);
    void flushUsefulRun();
    void insertUsefulRegion(Addr line, unsigned lines);
    void maybeClearBloom(unsigned filter);
    void resetConfidence(ThreadID tid);
    void notifyFTQRemove(const o3::FetchTargetPtr &ft);
    void notifyFTQSquash(const o3::FetchTargetPtr &ft);
    void notifyMispredict(const o3::DynInstPtr &inst);
    void notifyCommit(const o3::DynInstPtr &inst);
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_UDP_HH__
