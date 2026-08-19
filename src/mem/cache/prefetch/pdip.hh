/*
 * Copyright (c) 2026
 * All rights reserved.
 */

#ifndef __MEM_CACHE_PREFETCH_PDIP_HH__
#define __MEM_CACHE_PREFETCH_PDIP_HH__

#include <array>
#include <deque>
#include <list>
#include <unordered_map>
#include <vector>

#include "arch/generic/mmu.hh"
#include "base/random.hh"
#include "cpu/base.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/ftq.hh"
#include "mem/cache/base.hh"
#include "mem/cache/prefetch/base.hh"

namespace gem5
{

struct PriorityDirectedPrefetcherParams;

namespace prefetch
{

/**
 * Priority-directed instruction prefetcher. The table is read from the FTQ
 * insertion path, while its training information is collected from the
 * mispredict, commit, backend-stall, and L1I miss probes.
 */
class PriorityDirectedPrefetcher : public Base
{
  public:
    typedef PriorityDirectedPrefetcherParams Params;
    PriorityDirectedPrefetcher(const Params &p);
    ~PriorityDirectedPrefetcher() = default;

    void regProbeListeners() override;
    void setCache(BaseCache *_cache) { cache = _cache; }
    PacketPtr getPacket() override;
    Tick nextPrefetchReadyTime() const override;
    void notify(const CacheAccessProbeArg &, const PrefetchInfo &) override {}

  private:
    struct Target {
        Addr address = 0;
        // The base line is always prefetched. Bit n denotes the (n + 1)th
        // following line, as in the four-bit mask in the PDIP paper.
        uint64_t mask = 0;
    };
    struct Entry {
        bool valid = false;
        Addr tag = 0;
        // A one-bit NRU replacement marker, matching the paper's one bit of
        // replacement state per way (rather than a software-sized LRU time).
        bool nru = false;
        std::vector<Target> targets;
    };
    struct MissState {
        Addr pc = MaxAddr;
        Addr address = MaxAddr;
        Tick missTick = MaxTick;
        // Decode starvation is charged only while this demand miss is the
        // request blocking the O3 fetch stage.
        bool backendStalled = false;
        unsigned decodeStallCycles = 0;
        Tick lastDecodeStallTick = 0;
        bool finalized = false;
    };
    struct FTQState {
        o3::FTSeqNum ftn = 0;
        Addr start = 0;
        Addr end = 0;
        Addr trigger = MaxAddr;
        ThreadID tid = InvalidThreadID;
        bool resteerPath = false;
        std::vector<MissState> misses;
    };
    struct PrefetchRequest {
        PrefetchRequest(PriorityDirectedPrefetcher &owner, Addr addr,
                        ThreadID tid, o3::FTSeqNum ftn);
        const Addr addr;
        const o3::FTSeqNum ftn;
        RequestPtr req;
        PacketPtr pkt = nullptr;
        Tick readyTime = MaxTick;
        void createPkt();
    };

    void notifyFTQInsert(const o3::FetchTargetPtr &ft);
    void notifyFTQRemove(const o3::FetchTargetPtr &ft);
    void notifyFTQSquash(const o3::FetchTargetPtr &ft);
    void notifyCacheMiss(const CacheAccessProbeArg &arg);
    void notifyDecodeIcacheStall(const RequestPtr &req);
    void notifyMispredict(const o3::DynInstPtr &inst);
    void notifyCommit(const o3::DynInstPtr &inst);
    void notifyBackendStall(ThreadID tid);
    void finalizeFEC(FTQState &state, Addr retiredPC);
    void retirePendingMiss(Addr pc);
    void discardPendingMisses(const FTQState &state);
    FTQState *findFTQ(Addr pc, ThreadID tid);
    FTQState *findFTQByNum(o3::FTSeqNum ftn, ThreadID tid);
    void retireOldFTQs();
    void issueTargets(Addr trigger, ThreadID tid, o3::FTSeqNum ftn);
    void enqueue(Addr address, ThreadID tid, o3::FTSeqNum ftn);
    void train(Addr trigger, Addr target);
    MissState *findDecodeStallMiss(const RequestPtr &req);
    Entry *find(Addr trigger);
    const Entry *find(Addr trigger) const;
    size_t setIndex(Addr trigger) const;
    Addr tag(Addr trigger) const;
    bool sample(unsigned probability);

    std::vector<ProbeListenerPtr<>> listeners;
    BaseCPU *cpu;
    BaseCache *cache;
    const bool enabled;
    const unsigned tableSets;
    const unsigned tableAssoc;
    const unsigned tagBits;
    const unsigned targetAddressBits;
    const unsigned targetsPerEntry;
    const unsigned targetMaskBits;
    const unsigned trainingProbability;
    const unsigned minFreeMSHRs;
    const unsigned highCostCycles;
    const bool requireBackendStall;
    const bool ignoreReturns;
    const bool markReqAsPrefetch;
    const bool squashPrefetches;
    const bool cacheSnoop;
    const Tick latency;
    const unsigned pfqSize;
    const unsigned tqSize;
    Random::RandomPtr rng;
    std::vector<std::vector<Entry>> table;
    std::unordered_map<o3::FTSeqNum, FTQState> ftqs;
    // FTQ entries leave the queue before every instruction in the block
    // retires. Keep their metadata until commit makes the FEC decision.
    std::deque<FTQState> recentFtqs;
    std::unordered_map<o3::FTSeqNum, FTQState *> recentFtqIndex;
    static constexpr size_t RecentFTQWindow = 128;
    // Only instructions on these lines can finalize an FEC.  This keeps the
    // commit probe from scanning every live and recently consumed FTQ entry.
    std::unordered_map<Addr, unsigned> pendingMissBlocks;
    bool lastCommitFTQValid = false;
    o3::FTSeqNum lastCommitFTN = 0;
    Addr lastCommitStart = 0;
    Addr lastCommitEnd = 0;
    ThreadID lastCommitTid = InvalidThreadID;
    std::array<Addr, o3::MaxThreads> resteerTriggers;
    std::array<unsigned, o3::MaxThreads> resteerWindows;
    std::array<Addr, o3::MaxThreads> lastTakenBranches;
    std::list<PrefetchRequest> pfq;

    struct Stats : public statistics::Group {
        Stats(statistics::Group *parent);
        statistics::Scalar ftqInsertions;
        statistics::Scalar ftqRemovals;
        statistics::Scalar tableLookups;
        statistics::Scalar branchBlockLookups;
        statistics::Scalar nipLookups;
        statistics::Scalar tableInsertions;
        statistics::Scalar tableReplacements;
        statistics::Scalar instCacheMisses;
        statistics::Scalar missesMatchedFTQ;
        statistics::Scalar missesMatchedRecentFTQ;
        statistics::Scalar missesUnmatchedFTQ;
        statistics::Scalar finalizedFECs;
        statistics::Scalar retiredMisses;
        statistics::Scalar filteredLowCost;
        statistics::Scalar decodeStallEvents;
        statistics::Scalar filteredNoBackendStall;
        statistics::Scalar resteerTriggers;
        statistics::Scalar lastTakenTriggers;
        statistics::Scalar targetAddressOverflows;
        statistics::Scalar criticalBlocks;
        statistics::Scalar trainingEvents;
        statistics::Scalar predictorHits;
        statistics::Scalar predictorMisses;
        statistics::Scalar candidates;
        statistics::Scalar issued;
        statistics::Scalar squashed;
        statistics::Scalar droppedCacheSnoop;
        statistics::Scalar droppedMSHR;
        statistics::Scalar droppedQueue;
    } stats;
};

} // namespace prefetch
} // namespace gem5

#endif
