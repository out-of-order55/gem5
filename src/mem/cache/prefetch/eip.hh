/*
 * Copyright (c) 2026
 * All rights reserved.
 */

#ifndef __MEM_CACHE_PREFETCH_EIP_HH__
#define __MEM_CACHE_PREFETCH_EIP_HH__

#include <deque>
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
 * Cost-effective entangling instruction prefetcher.
 *
 * The implementation deliberately keeps the functional data structures
 * explicit (rather than modelling the bit-packed paper layout).  This makes
 * the prefetcher useful for experiments while exposing an estimate of the
 * corresponding logical storage in the statistics output.
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
    struct HistoryEntry {
        Addr head = 0;
        unsigned size = 0;
        Tick firstTick = 0;
    };

    struct Destination {
        Addr head = 0;
        unsigned size = 0;
        unsigned confidence = 0;
        Tick lastUpdate = 0;
    };

    struct TableEntry {
        bool valid = false;
        Addr source = 0;
        unsigned maxSize = 0;
        std::vector<Destination> destinations;
    };

    struct PendingPrefetch {
        Addr source = 0;
        Addr destination = 0;
        Tick issueTick = 0;
        bool filled = false;
    };

    struct QueuedPacket {
        PacketPtr pkt = nullptr;
        Tick readyTick = MaxTick;
        PendingPrefetch metadata;
    };

    struct Stats : public statistics::Group {
        Stats(statistics::Group *parent);
        statistics::Scalar basicBlocksObserved;
        statistics::Scalar historyInsertions;
        statistics::Scalar historyMerges;
        statistics::Scalar tableLookups;
        statistics::Scalar tableHits;
        statistics::Scalar tableInsertions;
        statistics::Scalar tableReplacements;
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
        statistics::Scalar coveredInstructionMisses;
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
    std::deque<HistoryEntry> history;
    std::deque<QueuedPacket> queue;
    std::unordered_map<Addr, PendingPrefetch> pending;
    std::vector<ProbeListenerPtr<>> cacheListeners;

    Addr currentHead = 0;
    Addr currentLastLine = 0;
    unsigned currentSize = 0;
    Tick currentFirstTick = 0;
    bool haveCurrent = false;

    unsigned tableSet(Addr source) const;
    TableEntry *findEntry(Addr source);
    const TableEntry *findEntry(Addr source) const;
    void finishCurrentBlock(Tick now);
    void insertHistory(const HistoryEntry &entry);
    void train(const HistoryEntry &entry);
    void issueFor(const PrefetchInfo &pfi, const CacheAccessor &cache);
    void enqueue(Addr address, Addr source, Addr destination,
                 const PrefetchInfo &pfi, const CacheAccessor &cache);
    void adjustConfidence(Addr source, Addr destination, bool increment);
    void removePending(Addr address);
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_EIP_HH__
