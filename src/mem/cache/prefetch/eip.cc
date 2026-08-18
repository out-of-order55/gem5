/*
 * Copyright (c) 2026
 * All rights reserved.
 */

#include "mem/cache/prefetch/eip.hh"

#include <algorithm>
#include <array>

#include "base/intmath.hh"
#include "base/logging.hh"
#include "mem/request.hh"
#include "params/EntanglingPrefetcher.hh"
#include "sim/system.hh"

namespace gem5
{
namespace prefetch
{

namespace
{

constexpr unsigned SourceTagBits = 10;
constexpr unsigned BasicBlockSizeBits = 6;
constexpr unsigned MaxPaperBasicBlockSize = (1U << BasicBlockSizeBits) - 1;
constexpr unsigned ConfidenceThreshold = 1;

}

EntanglingPrefetcher::EntanglingPrefetcher(const Params &p)
  : Base(p), stats(this), enabled(p.enabled), tableEntries(p.table_entries),
    tableAssoc(p.table_assoc), historyEntries(p.history_entries),
    maxBasicBlockSize(p.max_basic_block_size),
    destinationsPerEntry(p.destinations_per_entry),
    confidenceBits(p.confidence_bits), mergeDistance(p.merge_distance),
    queueSize(p.prefetch_queue_size), latency(p.latency),
    cacheSnoop(p.cache_snoop), table(p.table_entries),
    setVictim(std::max(1U, p.table_entries / p.table_assoc), 0),
    history(p.history_entries)
{
    fatal_if(tableAssoc != 16,
             "EntanglingPrefetcher requires the paper's 16-way table");
    fatal_if(tableEntries == 0 || tableEntries % tableAssoc != 0,
             "EntanglingPrefetcher table_entries must be a multiple of 16");
    const unsigned sets = tableEntries / tableAssoc;
    fatal_if((sets & (sets - 1)) != 0,
             "EntanglingPrefetcher table set count must be a power of two");
    fatal_if(historyEntries != 16,
             "EntanglingPrefetcher requires the paper's 16-entry history");
    fatal_if(maxBasicBlockSize != MaxPaperBasicBlockSize,
             "EntanglingPrefetcher basic-block size must use 6 bits (63)");
    fatal_if(destinationsPerEntry != PhysicalDestinationFormats,
             "Physical-address EIP requires four compressed destinations");
    fatal_if(confidenceBits != 2,
             "EntanglingPrefetcher requires 2-bit confidence counters");
    fatal_if(queueSize == 0, "EntanglingPrefetcher prefetch_queue_size must be > 0");

    // Physical-address table: 10-bit source tag, 6-bit block size, and a
    // 46-bit compressed destination field.  The timing metadata is modeled
    // separately, as in the paper's PQ/MSHR/L1I extensions.
    const unsigned setBits = floorLog2(sets);
    const uint64_t tableBits = uint64_t(tableEntries) *
        (SourceTagBits + BasicBlockSizeBits + 46);
    const uint64_t historyBits = uint64_t(historyEntries) * (42 + 20 + 6) + 4;
    const uint64_t timingBits = uint64_t(queueSize + 12 + 64) *
        (12 + 4 + setBits + 4 + 1);
    stats.logicalStorageBytes = (tableBits + historyBits + timingBits + 7) / 8;
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
             "Completed dynamic instruction basic blocks"),
    ADD_STAT(historyInsertions, statistics::units::Count::get(),
             "Basic-block heads inserted into the history buffer"),
    ADD_STAT(historyMerges, statistics::units::Count::get(),
             "Basic blocks merged into a recent history entry"),
    ADD_STAT(historyLookupHits, statistics::units::Count::get(),
             "Latency-qualified history sources"),
    ADD_STAT(historyLookupMisses, statistics::units::Count::get(),
             "Fills without a latency-qualified history source"),
    ADD_STAT(tableLookups, statistics::units::Count::get(),
             "Entangled table lookups"),
    ADD_STAT(tableHits, statistics::units::Count::get(),
             "Entangled table hits"),
    ADD_STAT(tableInsertions, statistics::units::Count::get(),
             "Entangled table insertions"),
    ADD_STAT(tableReplacements, statistics::units::Count::get(),
             "FIFO entangled table replacements"),
    ADD_STAT(tableRelocations, statistics::units::Count::get(),
             "FIFO victims reallocated to an empty destination entry"),
    ADD_STAT(entanglementInsertions, statistics::units::Count::get(),
             "Source/destination pairs inserted"),
    ADD_STAT(entanglementEvictions, statistics::units::Count::get(),
             "Destination pairs evicted by compression capacity"),
    ADD_STAT(prefetchCandidates, statistics::units::Count::get(),
             "Generated prefetch candidates"),
    ADD_STAT(prefetchIssued, statistics::units::Count::get(),
             "Issued EIP prefetches"),
    ADD_STAT(timelyPrefetches, statistics::units::Count::get(),
             "Demand hits on EIP-prefetched destination heads"),
    ADD_STAT(latePrefetches, statistics::units::Count::get(),
             "Demand misses coalescing with EIP prefetches"),
    ADD_STAT(wrongPrefetches, statistics::units::Count::get(),
             "Unused EIP-prefetched destination heads evicted"),
    ADD_STAT(confidenceIncrements, statistics::units::Count::get(),
             "Destination confidence increments"),
    ADD_STAT(confidenceDecrements, statistics::units::Count::get(),
             "Destination confidence decrements"),
    ADD_STAT(demandInstructionMisses, statistics::units::Count::get(),
             "Demand instruction misses observed"),
    ADD_STAT(fillTrainingEvents, statistics::units::Count::get(),
             "Miss fills that added an entanglement"),
    ADD_STAT(trainingWithoutHistory, statistics::units::Count::get(),
             "Miss fills without a valid history pointer"),
    ADD_STAT(prefetchDrops, statistics::units::Count::get(),
             "EIP packets discarded by the cache before MSHR allocation"),
    ADD_STAT(prefetchDropMetadataRetirements, statistics::units::Count::get(),
             "Discarded EIP packets whose timing metadata was retired"),
    ADD_STAT(queueHighWatermark, statistics::units::Count::get(),
             "Maximum EIP prefetch queue occupancy"),
    ADD_STAT(timingMshrHighWatermark, statistics::units::Count::get(),
             "Maximum EIP timing-MSHR metadata entries"),
    ADD_STAT(timingCacheHighWatermark, statistics::units::Count::get(),
             "Maximum EIP timing-cache metadata entries"),
    ADD_STAT(logicalStorageBytes, statistics::units::Byte::get(),
             "Paper-style physical-address EIP storage estimate")
{
}

unsigned
EntanglingPrefetcher::tableSet(Addr source) const
{
    const Addr line = source >> lBlkSize;
    const Addr hash = line ^ (line >> 2) ^ (line >> 5);
    return hash & ((tableEntries / tableAssoc) - 1);
}

Addr
EntanglingPrefetcher::tableTag(Addr source) const
{
    const Addr line = source >> lBlkSize;
    const Addr hash = line ^ (line >> 2) ^ (line >> 5);
    const unsigned setBits = floorLog2(tableEntries / tableAssoc);
    return (hash >> setBits) & ((Addr(1) << SourceTagBits) - 1);
}

Tick
EntanglingPrefetcher::currentCycle() const
{
    return curTick() / clockPeriod();
}

EntanglingPrefetcher::TableEntry *
EntanglingPrefetcher::findEntry(Addr source)
{
    const unsigned set = tableSet(source);
    const unsigned begin = set * tableAssoc;
    const Addr tag = tableTag(source);
    for (unsigned way = 0; way < tableAssoc; ++way) {
        auto &entry = table[begin + way];
        if (entry.valid && entry.tag == tag)
            return &entry;
    }
    return nullptr;
}

const EntanglingPrefetcher::TableEntry *
EntanglingPrefetcher::findEntry(Addr source) const
{
    return const_cast<EntanglingPrefetcher *>(this)->findEntry(source);
}

EntanglingPrefetcher::TableEntry &
EntanglingPrefetcher::allocateEntry(Addr source)
{
    const unsigned set = tableSet(source);
    const unsigned begin = set * tableAssoc;
    const unsigned way = setVictim[set];
    auto &victim = table[begin + way];

    if (victim.valid) {
        // Enhanced FIFO from the paper: relocate a FIFO victim to a way
        // without destinations, preferring a way without a block size.
        if (!victim.destinations.empty() || victim.size != 0) {
            TableEntry *relocation = nullptr;
            for (unsigned offset = 1; offset < tableAssoc; ++offset) {
                auto &candidate = table[begin + (way + offset) % tableAssoc];
                if (!candidate.valid || candidate.destinations.empty()) {
                    if (!candidate.valid || candidate.size == 0) {
                        relocation = &candidate;
                        break;
                    }
                    if (!relocation)
                        relocation = &candidate;
                }
            }
            if (relocation &&
                (!relocation->valid || relocation->size == 0 ||
                 !victim.destinations.empty())) {
                *relocation = victim;
                stats.tableRelocations++;
            }
        }
        stats.tableReplacements++;
        stats.entanglementEvictions += victim.destinations.size();
    } else {
        stats.tableInsertions++;
    }

    victim = TableEntry();
    victim.valid = true;
    victim.source = source;
    victim.tag = tableTag(source);
    setVictim[set] = (way + 1) % tableAssoc;
    return victim;
}

unsigned
EntanglingPrefetcher::destinationFormat(Addr source, Addr destination) const
{
    constexpr std::array<unsigned, PhysicalDestinationFormats> significant =
        {42, 20, 12, 9};
    const Addr sourceLine = source >> lBlkSize;
    const Addr destinationLine = destination >> lBlkSize;
    for (unsigned format = PhysicalDestinationFormats; format > 0; --format) {
        const unsigned bits = significant[format - 1];
        if ((sourceLine >> bits) == (destinationLine >> bits))
            return format;
    }
    return 1;
}

Addr
EntanglingPrefetcher::compressDestination(Addr destination,
                                          unsigned format) const
{
    constexpr std::array<unsigned, PhysicalDestinationFormats> significant =
        {42, 20, 12, 9};
    const unsigned bits = significant.at(format - 1);
    return (destination >> lBlkSize) & ((Addr(1) << bits) - 1);
}

Addr
EntanglingPrefetcher::expandDestination(Addr source, Addr encoded,
                                        unsigned format) const
{
    constexpr std::array<unsigned, PhysicalDestinationFormats> significant =
        {42, 20, 12, 9};
    const unsigned bits = significant.at(format - 1);
    const Addr sourceLine = source >> lBlkSize;
    return ((sourceLine & ~((Addr(1) << bits) - 1)) | encoded) << lBlkSize;
}

void
EntanglingPrefetcher::setDestinationFormat(TableEntry &entry,
                                           unsigned format)
{
    if (entry.format == format)
        return;

    for (auto &destination : entry.destinations) {
        const Addr head = expandDestination(entry.source, destination.encoded,
                                            entry.format);
        destination.encoded = compressDestination(head, format);
    }
    entry.format = format;
}

void
EntanglingPrefetcher::updateBasicBlock(Addr head, unsigned size)
{
    if (size == 0)
        return;
    auto *entry = findEntry(head);
    if (!entry)
        entry = &allocateEntry(head);
    entry->size = std::max(entry->size, size);
}

void
EntanglingPrefetcher::recomputeDestinationFormat(TableEntry &entry)
{
    if (entry.destinations.empty()) {
        entry.format = 1;
        return;
    }

    unsigned format = PhysicalDestinationFormats;
    for (const auto &destination : entry.destinations) {
        format = std::min(format,
            destinationFormat(entry.source, expandDestination(entry.source,
                destination.encoded, entry.format)));
    }
    setDestinationFormat(entry, format);
}

bool
EntanglingPrefetcher::canInsertWithoutEviction(Addr source,
                                                Addr destination) const
{
    const auto *entry = findEntry(source);
    if (!entry)
        return false;
    if (std::any_of(entry->destinations.begin(), entry->destinations.end(),
                    [this, source, entry, destination](
                        const Destination &known) {
                        return expandDestination(source, known.encoded,
                            entry->format) == destination;
                    })) {
        return true;
    }
    unsigned format = destinationFormat(source, destination);
    for (const auto &known : entry->destinations) {
        format = std::min(format, destinationFormat(source,
            expandDestination(source, known.encoded, entry->format)));
    }
    return entry->destinations.size() + 1 <= format;
}

void
EntanglingPrefetcher::addEntanglement(Addr source, Addr destination)
{
    auto *entry = findEntry(source);
    if (!entry)
        entry = &allocateEntry(source);

    const unsigned maxConfidence = (1U << confidenceBits) - 1;
    auto known = std::find_if(entry->destinations.begin(),
                              entry->destinations.end(),
                              [this, source, entry, destination](
                                  const Destination &item) {
                                  return expandDestination(source, item.encoded,
                                      entry->format) == destination;
                              });
    if (known != entry->destinations.end()) {
        known->confidence = maxConfidence;
        return;
    }

    unsigned format = destinationFormat(source, destination);
    for (const auto &knownDestination : entry->destinations) {
        format = std::min(format,
            destinationFormat(source, expandDestination(source,
                knownDestination.encoded, entry->format)));
    }
    while (entry->destinations.size() + 1 > format) {
        const auto victim = std::min_element(entry->destinations.begin(),
                                             entry->destinations.end(),
            [](const Destination &lhs, const Destination &rhs) {
                return lhs.confidence < rhs.confidence;
            });
        entry->destinations.erase(victim);
        stats.entanglementEvictions++;
        recomputeDestinationFormat(*entry);
        format = destinationFormat(source, destination);
        for (const auto &knownDestination : entry->destinations) {
            format = std::min(format,
                destinationFormat(source, expandDestination(source,
                    knownDestination.encoded, entry->format)));
        }
    }
    setDestinationFormat(*entry, format);
    entry->destinations.push_back(
        {compressDestination(destination, entry->format), maxConfidence});
    stats.entanglementInsertions++;
}

int
EntanglingPrefetcher::findHistory(Addr head) const
{
    for (unsigned count = 0; count < historyCount; ++count) {
        const unsigned index = (historyHead + historyEntries - 1 - count) %
            historyEntries;
        if (history[index].valid && history[index].head == head)
            return index;
    }
    return InvalidHistory;
}

int
EntanglingPrefetcher::insertHistory(Addr head)
{
    const unsigned position = historyHead;
    history[position] = {true, head, 0, currentCycle()};
    historyHead = (historyHead + 1) % historyEntries;
    historyCount = std::min(historyCount + 1, historyEntries);
    stats.historyInsertions++;
    return position;
}

void
EntanglingPrefetcher::updateHistorySize(Addr head, unsigned size)
{
    const int position = findHistory(head);
    if (position != InvalidHistory)
        history[position].size = std::max(history[position].size, size);
}

unsigned
EntanglingPrefetcher::findMergeOffset(Addr head) const
{
    const unsigned searches = std::min(mergeDistance, historyCount);
    for (unsigned count = 0; count < searches; ++count) {
        const unsigned index = (historyHead + historyEntries - 1 - count) %
            historyEntries;
        const auto &candidate = history[index];
        if (!candidate.valid || head <= candidate.head)
            continue;
        const Addr distance = head - candidate.head;
        if (distance % blkSize == 0 && distance / blkSize <= candidate.size)
            return distance / blkSize;
    }
    return 0;
}

int
EntanglingPrefetcher::beginBasicBlock(Addr line, bool isMiss)
{
    currentHead = line;
    currentLastLine = line;
    currentSize = 0;
    currentMergeOffset = findMergeOffset(line);
    haveCurrent = true;

    if (currentMergeOffset) {
        stats.historyMerges++;
        return InvalidHistory;
    }

    // Paper behavior: retain the first occurrence of a head, but retain a
    // fresh occurrence when a miss reaches the head and its outstanding
    // request has not already been accessed by a demand.
    const auto timing = timingMSHR.find(line);
    const bool alreadyAccessed = timing != timingMSHR.end() &&
        timing->second.accessed;
    if (findHistory(line) == InvalidHistory || (isMiss && !alreadyAccessed))
        return insertHistory(line);
    return InvalidHistory;
}

void
EntanglingPrefetcher::finishCurrentBlock()
{
    if (!haveCurrent)
        return;

    stats.basicBlocksObserved++;
    if (currentSize) {
        const Addr mergedHead = currentHead - currentMergeOffset * blkSize;
        const unsigned mergedSize = std::min(maxBasicBlockSize,
            currentSize + currentMergeOffset);
        updateBasicBlock(mergedHead, mergedSize);
        updateHistorySize(mergedHead, mergedSize);
    }
    haveCurrent = false;
}

int
EntanglingPrefetcher::observeBasicBlock(Addr line, bool isMiss)
{
    if (!haveCurrent)
        return beginBasicBlock(line, isMiss);
    if (line == currentLastLine)
        return InvalidHistory;
    if (line == currentLastLine + blkSize && currentSize < maxBasicBlockSize) {
        currentLastLine = line;
        ++currentSize;
        return InvalidHistory;
    }

    finishCurrentBlock();
    return beginBasicBlock(line, isMiss);
}

std::optional<Addr>
EntanglingPrefetcher::findTimelySource(Addr destination, int historyPos,
                                       Tick missStart, Tick missLatency,
                                       unsigned skip)
{
    if (historyPos == InvalidHistory || !history[historyPos].valid ||
        history[historyPos].head != destination) {
        stats.historyLookupMisses++;
        return std::nullopt;
    }

    unsigned skipped = 0;
    for (unsigned count = 1; count < historyCount; ++count) {
        const unsigned index = (historyPos + historyEntries - count) %
            historyEntries;
        const auto &candidate = history[index];
        if (!candidate.valid)
            continue;
        // A repeated destination in the retained window means the original
        // head was evicted and revisited; the paper does not entangle it.
        if (candidate.head == destination) {
            stats.historyLookupMisses++;
            return std::nullopt;
        }
        if (candidate.firstTick <= missStart &&
            missStart - candidate.firstTick >= missLatency) {
            if (skipped++ == skip) {
                stats.historyLookupHits++;
                return candidate.head;
            }
        }
    }
    stats.historyLookupMisses++;
    return std::nullopt;
}

void
EntanglingPrefetcher::trainAtFill(Addr destination,
                                  const TimingEntry &timing)
{
    if (!timing.accessed || timing.historyPos == InvalidHistory) {
        stats.trainingWithoutHistory++;
        return;
    }

    const Tick missLatency = currentCycle() - timing.issueTick;
    for (unsigned skip = 0; skip < 2; ++skip) {
        const auto source = findTimelySource(destination, timing.historyPos,
                                              timing.issueTick, missLatency,
                                              skip);
        if (source && *source != destination &&
            canInsertWithoutEviction(*source, destination)) {
            addEntanglement(*source, destination);
            stats.fillTrainingEvents++;
            return;
        }
    }

    const auto source = findTimelySource(destination, timing.historyPos,
                                          timing.issueTick, missLatency, 0);
    if (source && *source != destination) {
        addEntanglement(*source, destination);
        stats.fillTrainingEvents++;
    }
}

void
EntanglingPrefetcher::enqueue(Addr address, const SourceRef &source,
                              const PrefetchInfo &pfi,
                              const CacheAccessor &cache)
{
    const Addr line = blockAddress(address);
    const bool queued = std::any_of(queue.begin(), queue.end(),
        [line](const QueuedPacket &candidate) {
            return candidate.address == line;
        });
    if (queue.size() >= queueSize || queued || timingMSHR.count(line) ||
        timingCache.count(line)) {
        return;
    }
    if (cacheSnoop && (cache.inCache(line, pfi.isSecure()) ||
                       cache.inMissQueue(line, pfi.isSecure()))) {
        return;
    }

    RequestPtr req = std::make_shared<Request>(line, blkSize,
        Request::INST_FETCH, requestorId);
    req->setFlags(Request::PREFETCH);
    req->taskId(context_switch_task_id::Prefetcher);
    PacketPtr pkt = new Packet(req, MemCmd::HardPFReq);
    pkt->allocate();
    // PQ timing metadata becomes MSHR metadata only after the prefetch is
    // issued, matching Figure 4 of the paper.
    queue.push_back({pkt, line, curTick() + clockPeriod() * latency, source});
    if (stats.queueHighWatermark.value() < queue.size())
        stats.queueHighWatermark = queue.size();
}

void
EntanglingPrefetcher::issueFor(Addr line, const PrefetchInfo &pfi,
                               const CacheAccessor &cache)
{
    stats.tableLookups++;
    auto *entry = findEntry(line);
    if (!entry)
        return;
    stats.tableHits++;

    const unsigned set = tableSet(line);
    const unsigned way = entry - &table[set * tableAssoc];
    const SourceRef source{set, way, true};

    // The stored size is the number of lines following the source head.
    for (unsigned i = 1; i <= entry->size; ++i) {
        stats.prefetchCandidates++;
        enqueue(line + i * blkSize, SourceRef{}, pfi, cache);
    }

    for (const auto &destination : entry->destinations) {
        const Addr destinationHead = expandDestination(line,
            destination.encoded, entry->format);
        if (destination.confidence < ConfidenceThreshold ||
            destinationHead == line) {
            continue;
        }
        stats.tableLookups++;
        const auto *destinationEntry = findEntry(destinationHead);
        if (destinationEntry)
            stats.tableHits++;
        const unsigned size = destinationEntry ? destinationEntry->size : 0;
        for (unsigned i = 0; i <= size; ++i) {
            stats.prefetchCandidates++;
            // Only the destination head carries the source reference used
            // for confidence updates, matching the paper's timing metadata.
            enqueue(destinationHead + i * blkSize,
                    i == 0 ? source : SourceRef{}, pfi, cache);
        }
    }
}

void
EntanglingPrefetcher::adjustConfidence(const SourceRef &source,
                                       Addr destination, bool increment)
{
    if (!source.valid || source.set >= setVictim.size() ||
        source.way >= tableAssoc) {
        return;
    }
    auto &entry = table[source.set * tableAssoc + source.way];
    if (!entry.valid)
        return;
    const auto found = std::find_if(entry.destinations.begin(),
                                    entry.destinations.end(),
        [this, &entry, destination](const Destination &item) {
            return item.encoded == compressDestination(destination,
                                                        entry.format);
        });
    if (found == entry.destinations.end())
        return;

    const unsigned maxConfidence = (1U << confidenceBits) - 1;
    if (increment) {
        if (found->confidence < maxConfidence) {
            ++found->confidence;
            stats.confidenceIncrements++;
        }
    } else if (found->confidence) {
        --found->confidence;
        stats.confidenceDecrements++;
        if (!found->confidence) {
            entry.destinations.erase(found);
            recomputeDestinationFormat(entry);
            stats.entanglementEvictions++;
        }
    }
}

void
EntanglingPrefetcher::observeDemandMiss(Addr line, int historyPos)
{
    stats.demandInstructionMisses++;
    const auto existing = timingMSHR.find(line);
    if (existing == timingMSHR.end()) {
        timingMSHR.emplace(line,
            TimingEntry{currentCycle(), historyPos, SourceRef{}, true});
        if (stats.timingMshrHighWatermark.value() < timingMSHR.size())
            stats.timingMshrHighWatermark = timingMSHR.size();
        return;
    }

    auto &timing = existing->second;
    if (!timing.accessed) {
        stats.latePrefetches++;
        pfHitInMSHR();
        adjustConfidence(timing.source, line, false);
        timing.source.valid = false;
    }
    timing.accessed = true;
    timing.historyPos = historyPos;
}

void
EntanglingPrefetcher::notifyPrefetchDropped(const PacketPtr &pkt)
{
    if (pkt->req->requestorId() != requestorId)
        return;

    stats.prefetchDrops++;
    const Addr line = blockAddress(pkt->getAddr());
    if (timingMSHR.erase(line))
        stats.prefetchDropMetadataRetirements++;
}

void
EntanglingPrefetcher::observeDemandHit(Addr line)
{
    const auto existing = timingCache.find(line);
    if (existing == timingCache.end())
        return;

    auto &timing = existing->second;
    if (!timing.accessed && timing.source.valid) {
        stats.timelyPrefetches++;
        usefulPrefetches++;
        prefetchStats.pfUseful++;
        adjustConfidence(timing.source, line, true);
        timing.source.valid = false;
    }
    timing.accessed = true;
}

void
EntanglingPrefetcher::notify(const CacheAccessProbeArg &arg,
                             const PrefetchInfo &pfi)
{
    if (!enabled || arg.pkt->req->isPrefetch())
        return;

    const Addr line = blockAddress(pfi.getPaddr());
    const int historyPos = observeBasicBlock(line, pfi.isCacheMiss());
    if (pfi.isCacheMiss())
        observeDemandMiss(line, historyPos);
    else
        observeDemandHit(line);

    // A table lookup is keyed by the accessed line, so only a basic-block
    // head can trigger its own entanglements.
    issueFor(line, pfi, arg.cache);
}

void
EntanglingPrefetcher::notifyFill(const CacheAccessProbeArg &arg)
{
    const Addr line = blockAddress(arg.pkt->getAddr());
    const auto found = timingMSHR.find(line);
    if (found == timingMSHR.end())
        return;

    TimingEntry timing = found->second;
    timingMSHR.erase(found);
    trainAtFill(line, timing);
    // Only destination heads need to persist metadata in the timing-cache
    // table for timely/wrong confidence feedback.
    if (timing.source.valid)
        timingCache[line] = timing;
    if (stats.timingCacheHighWatermark.value() < timingCache.size())
        stats.timingCacheHighWatermark = timingCache.size();
}

void
EntanglingPrefetcher::notifyEvict(const CacheDataUpdateProbeArg &info)
{
    if (!info.newData.empty())
        return;

    const Addr line = blockAddress(info.addr);
    const auto found = timingCache.find(line);
    if (found == timingCache.end())
        return;

    if (!found->second.accessed && found->second.source.valid) {
        stats.wrongPrefetches++;
        prefetchUnused();
        adjustConfidence(found->second.source, line, false);
    }
    timingCache.erase(found);
}

PacketPtr
EntanglingPrefetcher::getPacket()
{
    if (queue.empty() || queue.front().readyTick > curTick())
        return nullptr;

    auto queued = queue.front();
    queue.pop_front();
    if (!timingMSHR.count(queued.address)) {
        timingMSHR.emplace(queued.address,
            TimingEntry{currentCycle(), InvalidHistory, queued.source, false});
        if (stats.timingMshrHighWatermark.value() < timingMSHR.size())
            stats.timingMshrHighWatermark = timingMSHR.size();
    }
    stats.prefetchIssued++;
    prefetchStats.pfIssued++;
    issuedPrefetches++;
    return queued.pkt;
}

Tick
EntanglingPrefetcher::nextPrefetchReadyTime() const
{
    return queue.empty() ? MaxTick : queue.front().readyTick;
}

} // namespace prefetch
} // namespace gem5
