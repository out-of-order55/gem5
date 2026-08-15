/*
 * Copyright (c) 2026
 * All rights reserved.
 */

#include "mem/cache/prefetch/eip.hh"

#include <algorithm>
#include <limits>

#include "base/logging.hh"
#include "mem/request.hh"
#include "params/EntanglingPrefetcher.hh"
#include "sim/system.hh"

namespace gem5
{
namespace prefetch
{

EntanglingPrefetcher::EntanglingPrefetcher(const Params &p)
  : Base(p), stats(this), enabled(p.enabled), tableEntries(p.table_entries),
    tableAssoc(p.table_assoc), historyEntries(p.history_entries),
    maxBasicBlockSize(p.max_basic_block_size),
    destinationsPerEntry(p.destinations_per_entry),
    confidenceBits(p.confidence_bits), mergeDistance(p.merge_distance),
    queueSize(p.prefetch_queue_size), latency(p.latency),
    cacheSnoop(p.cache_snoop),
    table(std::max(1u, static_cast<unsigned>(p.table_entries)),
          TableEntry()),
    setVictim(std::max(1u, static_cast<unsigned>(p.table_entries) /
                       std::max(1u, p.table_assoc)), 0)
{
    fatal_if(tableAssoc == 0, "EntanglingPrefetcher table_assoc must be > 0");
    fatal_if(historyEntries == 0, "EntanglingPrefetcher history_entries must be > 0");
    fatal_if(maxBasicBlockSize == 0,
             "EntanglingPrefetcher max_basic_block_size must be > 0");
    fatal_if(destinationsPerEntry == 0,
             "EntanglingPrefetcher destinations_per_entry must be > 0");
    fatal_if(confidenceBits == 0 || confidenceBits > 8,
             "EntanglingPrefetcher confidence_bits must be in [1, 8]");
    fatal_if(queueSize == 0, "EntanglingPrefetcher prefetch_queue_size must be > 0");
    const unsigned sets = std::max(1u, tableEntries / tableAssoc);
    table.resize(sets * tableAssoc);
    setVictim.resize(sets, 0);
    stats.logicalStorageBytes =
        (tableEntries * (64 + 8 + destinationsPerEntry * (64 + 8 + confidenceBits))
         + historyEntries * (64 + 8 + 64)) / 8;
}

EntanglingPrefetcher::~EntanglingPrefetcher()
{
    for (auto &entry : queue)
        delete entry.pkt;
}

void
EntanglingPrefetcher::regProbeListeners()
{
    if (!probeManager || !cacheListeners.empty())
        return;

    using CacheListener = ProbeListenerArgFunc<CacheAccessProbeArg>;
    cacheListeners.push_back(probeManager->connect<CacheListener>("Miss",
        [this](const auto &arg) {
            if (!arg.pkt->req->isPrefetch() && arg.pkt->req->hasPaddr()) {
                PrefetchInfo pfi(arg.pkt, arg.pkt->req->getPaddr(), true);
                notify(arg, pfi);
            }
        }));
    cacheListeners.push_back(probeManager->connect<CacheListener>("Hit",
        [this](const auto &arg) {
            if (!arg.pkt->req->isPrefetch() && arg.pkt->req->hasPaddr()) {
                PrefetchInfo pfi(arg.pkt, arg.pkt->req->getPaddr(), false);
                notify(arg, pfi);
            }
        }));
    cacheListeners.push_back(probeManager->connect<CacheListener>("Fill",
        [this](const auto &arg) { notifyFill(arg); }));
    using EvictListener = ProbeListenerArgFunc<CacheDataUpdateProbeArg>;
    cacheListeners.push_back(probeManager->connect<EvictListener>(
        "Data Update", [this](const auto &arg) { notifyEvict(arg); }));
}

EntanglingPrefetcher::Stats::Stats(statistics::Group *parent)
  : statistics::Group(parent),
    ADD_STAT(basicBlocksObserved, statistics::units::Count::get(),
             "Observed dynamic instruction basic blocks"),
    ADD_STAT(historyInsertions, statistics::units::Count::get(),
             "Basic blocks inserted into the history buffer"),
    ADD_STAT(historyMerges, statistics::units::Count::get(),
             "History entries merged by proximity"),
    ADD_STAT(tableLookups, statistics::units::Count::get(),
             "Entangled table lookups"),
    ADD_STAT(tableHits, statistics::units::Count::get(),
             "Entangled table hits"),
    ADD_STAT(tableInsertions, statistics::units::Count::get(),
             "Entangled table insertions"),
    ADD_STAT(tableReplacements, statistics::units::Count::get(),
             "Entangled table replacements"),
    ADD_STAT(entanglementInsertions, statistics::units::Count::get(),
             "Source/destination pairs inserted"),
    ADD_STAT(entanglementEvictions, statistics::units::Count::get(),
             "Source/destination pairs evicted"),
    ADD_STAT(prefetchCandidates, statistics::units::Count::get(),
             "Generated prefetch candidates"),
    ADD_STAT(prefetchIssued, statistics::units::Count::get(),
             "Issued EIP prefetches"),
    ADD_STAT(timelyPrefetches, statistics::units::Count::get(),
             "Demand accesses covered by EIP prefetches"),
    ADD_STAT(latePrefetches, statistics::units::Count::get(),
             "Late EIP prefetches"),
    ADD_STAT(wrongPrefetches, statistics::units::Count::get(),
             "Unused EIP prefetches evicted"),
    ADD_STAT(confidenceIncrements, statistics::units::Count::get(),
             "Destination confidence increments"),
    ADD_STAT(confidenceDecrements, statistics::units::Count::get(),
             "Destination confidence decrements"),
    ADD_STAT(demandInstructionMisses, statistics::units::Count::get(),
             "Demand instruction misses observed"),
    ADD_STAT(coveredInstructionMisses, statistics::units::Count::get(),
             "Demand instruction misses covered by EIP"),
    ADD_STAT(logicalStorageBytes, statistics::units::Byte::get(),
             "Estimated logical EIP storage")
{
}

unsigned
EntanglingPrefetcher::tableSet(Addr source) const
{
    const unsigned sets = table.size() / tableAssoc;
    return sets ? (static_cast<unsigned>((source >> lBlkSize) % sets)) : 0;
}

EntanglingPrefetcher::TableEntry *
EntanglingPrefetcher::findEntry(Addr source)
{
    const unsigned set = tableSet(source);
    const unsigned begin = set * tableAssoc;
    for (unsigned way = 0; way < tableAssoc && begin + way < table.size(); ++way) {
        auto &entry = table[begin + way];
        if (entry.valid && entry.source == source)
            return &entry;
    }
    return nullptr;
}

const EntanglingPrefetcher::TableEntry *
EntanglingPrefetcher::findEntry(Addr source) const
{
    return const_cast<EntanglingPrefetcher *>(this)->findEntry(source);
}

void
EntanglingPrefetcher::insertHistory(const HistoryEntry &entry)
{
    if (!history.empty() &&
        entry.head <= history.back().head + mergeDistance * blkSize &&
        entry.head + entry.size * blkSize >= history.back().head) {
        stats.historyMerges++;
    }
    history.push_back(entry);
    if (history.size() > historyEntries)
        history.pop_front();
    stats.historyInsertions++;
}

void
EntanglingPrefetcher::train(const HistoryEntry &entry)
{
    if (history.empty())
        return;
    const HistoryEntry *source = nullptr;
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
        if (it->firstTick <= entry.firstTick &&
            entry.head <= it->head + mergeDistance * blkSize &&
            entry.head + entry.size * blkSize >= it->head) {
            source = &*it;
            break;
        }
    }
    if (!source)
        source = &history.back();

    TableEntry *tableEntry = findEntry(source->head);
    if (!tableEntry) {
        const unsigned set = tableSet(source->head);
        const unsigned begin = set * tableAssoc;
        unsigned victim = setVictim[set]++ % tableAssoc;
        tableEntry = &table[begin + victim];
        if (tableEntry->valid) {
            stats.tableReplacements++;
            stats.entanglementEvictions += tableEntry->destinations.size();
        } else {
            stats.tableInsertions++;
        }
        *tableEntry = TableEntry();
        tableEntry->valid = true;
        tableEntry->source = source->head;
    }
    tableEntry->maxSize = std::max(tableEntry->maxSize, source->size);
    auto found = std::find_if(tableEntry->destinations.begin(),
                              tableEntry->destinations.end(),
                              [&entry](const Destination &d) {
                                  return d.head == entry.head;
                              });
    const unsigned maxConfidence = (1u << confidenceBits) - 1;
    if (found != tableEntry->destinations.end()) {
        found->size = entry.size;
        found->confidence = maxConfidence;
        found->lastUpdate = curTick();
    } else {
        if (tableEntry->destinations.size() >= destinationsPerEntry) {
            auto victim = std::min_element(
                tableEntry->destinations.begin(), tableEntry->destinations.end(),
                [](const Destination &a, const Destination &b) {
                    return a.confidence < b.confidence;
                });
            if (victim != tableEntry->destinations.end()) {
                stats.entanglementEvictions++;
                *victim = Destination{entry.head, entry.size, maxConfidence,
                                      curTick()};
            }
        } else {
            tableEntry->destinations.push_back(
                Destination{entry.head, entry.size, maxConfidence, curTick()});
        }
        stats.entanglementInsertions++;
    }
}

void
EntanglingPrefetcher::finishCurrentBlock(Tick now)
{
    if (!haveCurrent || currentSize == 0)
        return;
    HistoryEntry entry{currentHead, currentSize, currentFirstTick};
    stats.basicBlocksObserved++;
    train(entry);
    insertHistory(entry);
    currentLastLine = 0;
    currentSize = 0;
    currentHead = 0;
    currentFirstTick = now;
    haveCurrent = false;
}

void
EntanglingPrefetcher::enqueue(Addr address, Addr source, Addr destination,
                              const PrefetchInfo &pfi,
                              const CacheAccessor &cache)
{
    if (queue.size() >= queueSize || pending.count(address))
        return;
    if (cacheSnoop && (cache.inCache(address, pfi.isSecure()) ||
                       cache.inMissQueue(address, pfi.isSecure())))
        return;
    RequestPtr req = std::make_shared<Request>(address, blkSize,
        Request::INST_FETCH, requestorId);
    req->setFlags(Request::PREFETCH);
    req->taskId(context_switch_task_id::Prefetcher);
    PacketPtr pkt = new Packet(req, MemCmd::HardPFReq);
    pkt->allocate();
    PendingPrefetch meta{source, destination, curTick(), false};
    queue.push_back(QueuedPacket{pkt, curTick() + clockPeriod() * latency, meta});
    pending[address] = meta;
}

void
EntanglingPrefetcher::issueFor(const PrefetchInfo &pfi,
                               const CacheAccessor &cache)
{
    TableEntry *entry = findEntry(currentHead);
    stats.tableLookups++;
    if (!entry)
        return;
    stats.tableHits++;
    const Addr line = blockAddress(pfi.getPaddr());
    const Addr end = currentHead +
        std::min(currentSize, entry->maxSize) * blkSize;
    for (Addr addr = line + blkSize; addr < end; addr += blkSize) {
        stats.prefetchCandidates++;
        enqueue(addr, currentHead, currentHead, pfi, cache);
    }
    for (const auto &dst : entry->destinations) {
        if (!dst.confidence)
            continue;
        for (unsigned i = 0; i < dst.size; ++i) {
            const Addr addr = dst.head + i * blkSize;
            stats.prefetchCandidates++;
            enqueue(addr, currentHead, dst.head, pfi, cache);
        }
    }
}

void
EntanglingPrefetcher::adjustConfidence(Addr source, Addr destination,
                                       bool increment)
{
    TableEntry *entry = findEntry(source);
    if (!entry)
        return;
    auto it = std::find_if(entry->destinations.begin(), entry->destinations.end(),
                           [destination](const Destination &d) {
                               return d.head == destination;
                           });
    if (it == entry->destinations.end())
        return;
    const unsigned maxConfidence = (1u << confidenceBits) - 1;
    if (increment) {
        if (it->confidence < maxConfidence) {
            ++it->confidence;
            stats.confidenceIncrements++;
        }
    } else if (it->confidence) {
        --it->confidence;
        stats.confidenceDecrements++;
        if (!it->confidence) {
            entry->destinations.erase(it);
            stats.entanglementEvictions++;
        }
    }
}

void
EntanglingPrefetcher::removePending(Addr address)
{
    pending.erase(blockAddress(address));
}

void
EntanglingPrefetcher::notify(const CacheAccessProbeArg &arg,
                             const PrefetchInfo &pfi)
{
    // Child prefetchers may not receive the normal startup callback when
    // attached through MultiPrefetcher, so publish this derived statistic on
    // the first observed cache event after the stats reset.
    stats.logicalStorageBytes =
        (tableEntries * (64 + 8 + destinationsPerEntry *
                         (64 + 8 + confidenceBits)) +
         historyEntries * (64 + 8 + 64)) / 8;
    // EIP is attached only to L1I.  The cache probe packet can lose the
    // INST_FETCH request flag while retaining the instruction-cache
    // requestor, so the cache binding itself is the instruction filter.
    if (!enabled || arg.pkt->req->isPrefetch())
        return;
    const Addr line = blockAddress(pfi.getPaddr());
    auto pendingIt = pending.find(line);
    const bool covered = pendingIt != pending.end() &&
        arg.cache.hasBeenPrefetched(line, arg.pkt->isSecure(), requestorId);
    if (pfi.isCacheMiss()) {
        stats.demandInstructionMisses++;
        if (pendingIt != pending.end()) {
            stats.latePrefetches++;
            pfHitInMSHR();
            adjustConfidence(pendingIt->second.source,
                             pendingIt->second.destination, false);
            removePending(line);
        }
    } else if (covered) {
        stats.timelyPrefetches++;
        stats.coveredInstructionMisses++;
        usefulPrefetches++;
        prefetchStats.pfUseful++;
        adjustConfidence(pendingIt->second.source,
                         pendingIt->second.destination, true);
        removePending(line);
    }

    if (!haveCurrent) {
        currentHead = line;
        currentLastLine = line;
        currentSize = 1;
        currentFirstTick = curTick();
        haveCurrent = true;
    } else if (line == currentLastLine + blkSize &&
               currentSize < maxBasicBlockSize) {
        currentLastLine = line;
        ++currentSize;
    } else if (line != currentLastLine) {
        finishCurrentBlock(curTick());
        currentHead = line;
        currentLastLine = line;
        currentSize = 1;
        currentFirstTick = curTick();
        haveCurrent = true;
    }
    issueFor(pfi, arg.cache);
}

void
EntanglingPrefetcher::notifyFill(const CacheAccessProbeArg &arg)
{
    if (arg.pkt->req->isPrefetch()) {
        auto it = pending.find(blockAddress(arg.pkt->getAddr()));
        if (it != pending.end())
            it->second.filled = true;
    }
}

void
EntanglingPrefetcher::notifyEvict(const CacheDataUpdateProbeArg &info)
{
    if (!info.hwPrefetched)
        return;
    const Addr line = blockAddress(info.addr);
    auto it = pending.find(line);
    if (it != pending.end()) {
        stats.wrongPrefetches++;
        adjustConfidence(it->second.source, it->second.destination, false);
        removePending(line);
    }
}

PacketPtr
EntanglingPrefetcher::getPacket()
{
    if (queue.empty() || queue.front().readyTick > curTick())
        return nullptr;
    PacketPtr pkt = queue.front().pkt;
    queue.pop_front();
    stats.prefetchIssued++;
    prefetchStats.pfIssued++;
    issuedPrefetches++;
    return pkt;
}

Tick
EntanglingPrefetcher::nextPrefetchReadyTime() const
{
    return queue.empty() ? MaxTick : queue.front().readyTick;
}

} // namespace prefetch
} // namespace gem5
