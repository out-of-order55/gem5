/*
 * Copyright (c) 2026
 * All rights reserved.
 */

#ifndef __MEM_CACHE_PREFETCH_EIP_HH__
#define __MEM_CACHE_PREFETCH_EIP_HH__

#include <deque>
#include <optional>
#include <unordered_map>
#include <vector>

#include "base/statistics.hh"
#include "base/types.hh"
#include "mem/cache/prefetch/base.hh"

namespace gem5
{

struct EntanglingPrefetcherParams;

namespace prefetch
{

/**
 * ISCA'21 Entangling Instruction Prefetcher.
 *
 * The cache timing metadata mirrors the paper's PQ/MSHR/L1I extensions.
 * Keeping it locally avoids changing the generic gem5 MSHR implementation
 * while preserving the paper's fill-time, latency-aware training semantics.
 */
class EntanglingPrefetcher : public Base
{
  public:
    using Params = EntanglingPrefetcherParams;

    EntanglingPrefetcher(const Params &p);
    ~EntanglingPrefetcher() override;

    void regProbeListeners() override;
    void notify(const CacheAccessProbeArg &arg,
                const PrefetchInfo &pfi) override;
    void notifyFill(const CacheAccessProbeArg &arg) override;
    void notifyEvict(const CacheDataUpdateProbeArg &info) override;
    PacketPtr getPacket() override;
    Tick nextPrefetchReadyTime() const override;

  private:
    static constexpr int InvalidHistory = -1;
    static constexpr unsigned PhysicalDestinationFormats = 4;

    struct HistoryEntry {
        bool valid = false;
        Addr head = 0;
        unsigned size = 0;
        Tick firstTick = 0;
    };

    struct Destination {
        Addr encoded = 0;
        unsigned confidence = 0;
    };

    struct TableEntry {
        bool valid = false;
        Addr source = 0;
        Addr tag = 0;
        unsigned size = 0;
        unsigned format = 1;
        std::vector<Destination> destinations;
    };

    struct SourceRef {
        unsigned set = 0;
        unsigned way = 0;
        bool valid = false;
    };

    struct TimingEntry {
        Tick issueTick = 0;
        int historyPos = InvalidHistory;
        SourceRef source;
        bool accessed = false;
    };

    struct QueuedPacket {
        PacketPtr pkt = nullptr;
        Addr address = 0;
        Tick readyTick = MaxTick;
        SourceRef source;
    };

    struct Stats : public statistics::Group {
        Stats(statistics::Group *parent);
        statistics::Scalar basicBlocksObserved;
        statistics::Scalar historyInsertions;
        statistics::Scalar historyMerges;
        statistics::Scalar historyLookupHits;
        statistics::Scalar historyLookupMisses;
        statistics::Scalar tableLookups;
        statistics::Scalar tableHits;
        statistics::Scalar tableInsertions;
        statistics::Scalar tableReplacements;
        statistics::Scalar tableRelocations;
        statistics::Scalar entanglementInsertions;
        statistics::Scalar entanglementEvictions;
        statistics::Scalar prefetchCandidates;
        statistics::Scalar prefetchIssued;
        statistics::Scalar timelyPrefetches;
        statistics::Scalar latePrefetches;
        statistics::Scalar wrongPrefetches;
        statistics::Scalar confidenceIncrements;
        statistics::Scalar confidenceDecrements;
        statistics::Scalar demandInstructionMisses;
        statistics::Scalar fillTrainingEvents;
        statistics::Scalar trainingWithoutHistory;
        statistics::Scalar logicalStorageBytes;
    } stats;

    const bool enabled;
    const unsigned tableEntries;
    const unsigned tableAssoc;
    const unsigned historyEntries;
    const unsigned maxBasicBlockSize;
    const unsigned destinationsPerEntry;
    const unsigned confidenceBits;
    const unsigned mergeDistance;
    const unsigned queueSize;
    const Cycles latency;
    const bool cacheSnoop;

    std::vector<TableEntry> table;
    std::vector<unsigned> setVictim;
    std::vector<HistoryEntry> history;
    unsigned historyHead = 0;
    unsigned historyCount = 0;
    std::deque<QueuedPacket> queue;
    std::unordered_map<Addr, TimingEntry> timingMSHR;
    std::unordered_map<Addr, TimingEntry> timingCache;
    std::vector<ProbeListenerPtr<>> cacheListeners;

    Addr currentHead = 0;
    Addr currentLastLine = 0;
    unsigned currentSize = 0;
    unsigned currentMergeOffset = 0;
    bool haveCurrent = false;

    unsigned tableSet(Addr source) const;
    Addr tableTag(Addr source) const;
    Tick currentCycle() const;
    TableEntry *findEntry(Addr source);
    const TableEntry *findEntry(Addr source) const;
    TableEntry &allocateEntry(Addr source);
    unsigned destinationFormat(Addr source, Addr destination) const;
    Addr compressDestination(Addr destination, unsigned format) const;
    Addr expandDestination(Addr source, Addr encoded, unsigned format) const;
    void setDestinationFormat(TableEntry &entry, unsigned format);
    void recomputeDestinationFormat(TableEntry &entry);
    void updateBasicBlock(Addr head, unsigned size);
    bool canInsertWithoutEviction(Addr source, Addr destination) const;
    void addEntanglement(Addr source, Addr destination);

    int findHistory(Addr head) const;
    int insertHistory(Addr head);
    void updateHistorySize(Addr head, unsigned size);
    unsigned findMergeOffset(Addr head) const;
    int beginBasicBlock(Addr line, bool isMiss);
    int observeBasicBlock(Addr line, bool isMiss);
    void finishCurrentBlock();
    std::optional<Addr> findTimelySource(Addr destination, int historyPos,
                                         Tick missStart, Tick missLatency,
                                         unsigned skip);
    void trainAtFill(Addr destination, const TimingEntry &timing);

    void issueFor(Addr line, const PrefetchInfo &pfi,
                  const CacheAccessor &cache);
    void enqueue(Addr address, const SourceRef &source,
                 const PrefetchInfo &pfi, const CacheAccessor &cache);
    void adjustConfidence(const SourceRef &source, Addr destination,
                          bool increment);
    void observeDemandMiss(Addr line, int historyPos);
    void observeDemandHit(Addr line);
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_EIP_HH__
