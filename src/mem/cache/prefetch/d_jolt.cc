/*
 * Copyright (c) 2026
 * All rights reserved.
 *
 * D-JOLT follows the public IPC-1 implementation in Scarab:
 * src/prefetcher/D_JOLT.cc (Nakamura et al.).
 */

#include "mem/cache/prefetch/d_jolt.hh"

#include <algorithm>
#include <limits>

#include "base/logging.hh"
#include "mem/request.hh"
#include "params/DistantJoltPrefetcher.hh"

namespace gem5
{
namespace prefetch
{

bool
DistantJoltPrefetcher::MissInfo::add(const CompressedAddress &address)
{
    if (!base.valid()) {
        base = address;
        return true;
    }
    if (base.upper != address.upper || address.lower < base.lower)
        return false;
    const uint64_t distance = address.lower - base.lower;
    if (distance == 0)
        return true;
    if (distance > 8)
        return false;
    bits |= uint8_t(1U << (distance - 1));
    return true;
}

std::vector<DistantJoltPrefetcher::CompressedAddress>
DistantJoltPrefetcher::MissInfo::addresses() const
{
    std::vector<CompressedAddress> result;
    if (!base.valid())
        return result;
    result.push_back(base);
    for (unsigned bit = 0; bit < 8; ++bit) {
        if (bits & (1U << bit)) {
            auto address = base;
            address.lower += bit + 1;
            result.push_back(address);
        }
    }
    return result;
}

std::pair<bool, DistantJoltPrefetcher::CompressedAddress>
DistantJoltPrefetcher::UpperBitTable::compress(Addr addr, unsigned line_bits)
{
    const uint64_t upper = addr & UpperBitMask;
    const uint64_t lower = (addr & ~UpperBitMask) >> line_bits;
    for (unsigned i = 0; i < entries.size(); ++i) {
        if (entries[i].valid && entries[i].upper == upper)
            return {true, {i + 1, lower}};
    }
    for (unsigned i = 0; i < entries.size(); ++i) {
        if (!entries[i].valid) {
            entries[i] = {true, upper};
            return {true, {i + 1, lower}};
        }
    }
    return {false, {}};
}

Addr
DistantJoltPrefetcher::UpperBitTable::decompress(
    const CompressedAddress &address, unsigned line_bits) const
{
    assert(address.valid());
    return entries.at(address.upper - 1).upper + (address.lower << line_bits);
}

DistantJoltPrefetcher::SignatureGenerator::SignatureGenerator(unsigned length)
  : history(length)
{
}

uint32_t
DistantJoltPrefetcher::SignatureGenerator::makeSignature() const
{
    constexpr uint32_t mask = (1U << SignatureBits) - 1;
    uint32_t signature = 0;
    for (unsigned i = 0; i < history.size(); ++i) {
        const uint32_t pc = history[(head + i) % history.size()];
        signature = (signature << (SignatureBits - 5)) | (signature >> 5);
        signature ^= pc ^ (pc >> 2);
        signature &= mask;
    }
    signature ^= uint32_t(returns * 0xabcdULL);
    return signature & mask;
}

uint32_t
DistantJoltPrefetcher::SignatureGenerator::onCall(Addr pc)
{
    returns = 0;
    history[head] = uint32_t(pc);
    head = (head + 1) % history.size();
    return makeSignature();
}

uint32_t
DistantJoltPrefetcher::SignatureGenerator::onReturn()
{
    ++returns;
    return makeSignature();
}

DistantJoltPrefetcher::SignatureQueue::SignatureQueue(unsigned distance)
  : entries(distance)
{
}

void
DistantJoltPrefetcher::SignatureQueue::push(uint32_t signature)
{
    entries[(head + 1) % entries.size()] = signature;
    head = (head + 1) % entries.size();
}

uint32_t
DistantJoltPrefetcher::SignatureQueue::delayed() const
{
    return entries[(head + 1) % entries.size()];
}

DistantJoltPrefetcher::MissTable::MissTable(unsigned _sets, unsigned _ways)
  : sets(_sets), ways(_ways), entries(_sets * _ways)
{
    fatal_if(sets == 0 || ways == 0 || (sets & (sets - 1)) != 0,
             "D-JOLT miss-table sets must be non-zero powers of two");
}

DistantJoltPrefetcher::MissTable::Entry *
DistantJoltPrefetcher::MissTable::find(uint32_t signature)
{
    const unsigned begin = (signature & (sets - 1)) * ways;
    for (unsigned way = 0; way < ways; ++way) {
        auto &entry = entries[begin + way];
        if (entry.valid && entry.signature == signature)
            return &entry;
    }
    return nullptr;
}

const DistantJoltPrefetcher::MissTable::Entry *
DistantJoltPrefetcher::MissTable::find(uint32_t signature) const
{
    const unsigned begin = (signature & (sets - 1)) * ways;
    for (unsigned way = 0; way < ways; ++way) {
        const auto &entry = entries[begin + way];
        if (entry.valid && entry.signature == signature)
            return &entry;
    }
    return nullptr;
}

DistantJoltPrefetcher::MissTable::Entry &
DistantJoltPrefetcher::MissTable::allocate(uint32_t signature)
{
    const unsigned begin = (signature & (sets - 1)) * ways;
    Entry *victim = &entries[begin];
    for (unsigned way = 0; way < ways; ++way) {
        auto &entry = entries[begin + way];
        if (!entry.valid) {
            victim = &entry;
            break;
        }
        if (entry.touch < victim->touch)
            victim = &entry;
    }
    *victim = {};
    victim->valid = true;
    victim->signature = signature;
    victim->touch = ++sequence;
    return *victim;
}

bool
DistantJoltPrefetcher::MissTable::insert(uint32_t signature,
                                          const CompressedAddress &address)
{
    auto *entry = find(signature);
    if (!entry)
        entry = &allocate(signature);
    entry->touch = ++sequence;
    for (auto &vector : entry->vectors) {
        if (vector.add(address))
            return true;
    }
    return false;
}

void
DistantJoltPrefetcher::MissTable::touch(uint32_t signature)
{
    if (auto *entry = find(signature))
        entry->touch = ++sequence;
}

std::vector<DistantJoltPrefetcher::CompressedAddress>
DistantJoltPrefetcher::MissTable::lookup(uint32_t signature) const
{
    std::vector<CompressedAddress> result;
    const auto *entry = find(signature);
    if (!entry)
        return result;
    for (const auto &vector : entry->vectors) {
        auto addresses = vector.addresses();
        result.insert(result.end(), addresses.begin(), addresses.end());
    }
    return result;
}

DistantJoltPrefetcher::StreamPrefetcher::StreamPrefetcher(
    DistantJoltPrefetcher &_owner)
  : owner(_owner)
{
}

void
DistantJoltPrefetcher::StreamPrefetcher::allocateTraining(Addr line)
{
    auto victim = std::min_element(training.begin(), training.end(),
        [](const auto &a, const auto &b) {
            return (!a.valid && b.valid) || (a.valid == b.valid && a.touch < b.touch);
        });
    *victim = {true, line, 0, ++sequence};
}

void
DistantJoltPrefetcher::StreamPrefetcher::allocateMonitoring(Addr line)
{
    auto victim = std::min_element(monitoring.begin(), monitoring.end(),
        [](const auto &a, const auto &b) {
            return (!a.valid && b.valid) || (a.valid == b.valid && a.touch < b.touch);
        });
    *victim = {true, line, ++sequence};
}

void
DistantJoltPrefetcher::StreamPrefetcher::access(Addr line, bool miss,
                                                   bool prefetch_hit,
                                                   const CacheAccessor &cache)
{
    for (auto &entry : monitoring) {
        if (entry.valid && line >= entry.start && line < entry.start + 2) {
            for (unsigned i = 0; i < 2; ++i) {
                owner.enqueue((entry.start + 2) << owner.lBlkSize,
                              &cache, true, false);
                ++entry.start;
            }
            entry.touch = ++sequence;
            return;
        }
    }

    if (!miss && !prefetch_hit)
        return;
    for (auto &entry : training) {
        if (!entry.valid || line < entry.start || line >= entry.start + 2)
            continue;
        ++entry.count;
        if (entry.count >= 3) {
            owner.enqueue((line + 1) << owner.lBlkSize,
                          &cache, true, true);
            allocateMonitoring(line);
            entry.valid = false;
        } else {
            entry.touch = ++sequence;
        }
        return;
    }
    allocateTraining(line);
}

DistantJoltPrefetcher::DistantJoltPrefetcher(const Params &p)
  : Base(p), stats(this), cpu(p.cpu), queueSize(p.prefetch_queue_size),
    latency(cyclesToTicks(p.latency)), cacheSnoop(p.cache_snoop),
    shortGenerator(p.short_history_length), longGenerator(p.long_history_length),
    shortHistory(p.short_distance), longHistory(p.long_distance),
    shortMissTable(p.short_table_sets, p.table_assoc),
    longMissTable(p.long_table_sets, p.table_assoc),
    extraMissTable(p.extra_table_sets, p.table_assoc), stream(*this)
{
    fatal_if(cpu == nullptr, "D-JOLT requires an O3 CPU");
    fatal_if(p.short_history_length != 4 || p.long_history_length != 7 ||
             p.short_distance != 4 || p.long_distance != 15,
             "D-JOLT parameters must use the published (4,4) and (7,15) pairs");
    fatal_if(p.table_assoc != 4 || p.short_table_sets != 32 ||
             p.long_table_sets != 64 || p.extra_table_sets != 256,
             "D-JOLT uses the public 8 KiB Scarab table configuration");
}

DistantJoltPrefetcher::~DistantJoltPrefetcher()
{
    for (auto &entry : queue)
        delete entry.packet;
}

void
DistantJoltPrefetcher::regProbeListeners()
{
    if (!probeListeners.empty())
        return;
    using InstListener = ProbeListenerArgFunc<o3::DynInstPtr>;
    probeListeners.push_back(cpu->getProbeManager()->connect<InstListener>(
        "Fetch", [this](const auto &inst) { onFetch(inst); }));

    fatal_if(probeManager == nullptr,
             "D-JOLT must be attached to an instruction cache");
    using CacheListener = ProbeListenerArgFunc<CacheAccessProbeArg>;
    probeListeners.push_back(probeManager->connect<CacheListener>("Miss",
        [this](const auto &arg) {
            if (!arg.pkt->req->isPrefetch() && arg.pkt->req->isInstFetch() &&
                arg.pkt->req->hasPaddr()) {
                PrefetchInfo pfi(arg.pkt, arg.pkt->req->getPaddr(), true);
                notify(arg, pfi);
            }
        }));
    probeListeners.push_back(probeManager->connect<CacheListener>("Hit",
        [this](const auto &arg) {
            if (!arg.pkt->req->isPrefetch() && arg.pkt->req->isInstFetch() &&
                arg.pkt->req->hasPaddr()) {
                PrefetchInfo pfi(arg.pkt, arg.pkt->req->getPaddr(), false);
                notify(arg, pfi);
            }
        }));
}

void
DistantJoltPrefetcher::onFetch(const o3::DynInstPtr &inst)
{
    // Scarab's public D-JOLT implementation updates FIFO_RETCNT for
    // CF_CBR, CF_IBR, CF_REP, and CF_RET only.  These map to gem5's
    // conditional/indirect controls and returns; direct calls and jumps do
    // not update the signature.
    if (!inst || !(inst->isCondCtrl() || inst->isIndirectCtrl() ||
                   inst->isReturn()))
        return;
    stats.fetchControlInstructions++;
    const uint32_t short_sig = inst->isReturn()
        ? shortGenerator.onReturn()
        : shortGenerator.onCall(inst->pcState().instAddr());
    const uint32_t long_sig = inst->isReturn()
        ? longGenerator.onReturn()
        : longGenerator.onCall(inst->pcState().instAddr());
    shortHistory.push(short_sig);
    longHistory.push(long_sig);
    prefetchFor(shortMissTable, short_sig, nullptr);
    prefetchFor(extraMissTable, short_sig, nullptr);
    prefetchFor(longMissTable, long_sig, nullptr);
    prefetchFor(extraMissTable, long_sig, nullptr);
}

void
DistantJoltPrefetcher::learn(MissTable &table, uint32_t signature,
                              const CompressedAddress &address)
{
    if (!table.insert(signature, address)) {
        extraMissTable.insert(signature, address);
        stats.extraMissLearning++;
    } else {
        // Scarab keeps an existing overflow entry recent even when the
        // current address fits in the primary table.
        extraMissTable.touch(signature);
        stats.missTableLearning++;
    }
}

void
DistantJoltPrefetcher::accessCache(Addr addr, bool miss, bool prefetch_hit,
                                    const CacheAccessor &cache)
{
    // Scarab's windowed stream component trains in cache-line units.  Keep
    // that representation local to the stream predictor; the signature miss
    // tables below retain byte addresses for upper-bit compression.
    stream.access(addr >> lBlkSize, miss, prefetch_hit, cache);
    if (!miss)
        return;
    stats.cacheMisses++;
    const auto compressed = upperBits.compress(blockAddress(addr), lBlkSize);
    if (!compressed.first) {
        stats.upperBitTableFull++;
        return;
    }
    learn(shortMissTable, shortHistory.delayed(), compressed.second);
    learn(longMissTable, longHistory.delayed(), compressed.second);
}

void
DistantJoltPrefetcher::notify(const CacheAccessProbeArg &acc,
                               const PrefetchInfo &pfi)
{
    const bool prefetch_hit = acc.cache.hasBeenPrefetched(
        pfi.getPaddr(), pfi.isSecure());
    accessCache(pfi.getPaddr(), pfi.isCacheMiss(), prefetch_hit, acc.cache);
}

void
DistantJoltPrefetcher::prefetchFor(const MissTable &table, uint32_t signature,
                                    const CacheAccessor *cache)
{
    stats.signatureLookups++;
    for (const auto &compressed : table.lookup(signature)) {
        enqueue(upperBits.decompress(compressed, lBlkSize), cache, false, false);
        stats.signaturePrefetches++;
    }
}

void
DistantJoltPrefetcher::enqueue(Addr address, const CacheAccessor *cache,
                                bool stream_request, bool initial_stream)
{
    const Addr line = blockAddress(address);
    if (queuedAddresses.count(line))
        return;
    if (cacheSnoop && cache && (cache->inCache(line, false) ||
                                cache->inMissQueue(line, false))) {
        stats.cacheSnoopDrops++;
        return;
    }
    if (queue.size() >= queueSize) {
        stats.queueDrops++;
        return;
    }
    auto request = std::make_shared<Request>(line, blkSize, Request::INST_FETCH,
                                             requestorId);
    request->setFlags(Request::PREFETCH);
    request->taskId(context_switch_task_id::Prefetcher);
    auto *packet = new Packet(request, MemCmd::HardPFReq);
    packet->allocate();
    queue.push_back({packet, line, curTick() + latency});
    queuedAddresses.insert(line);
    if (stream_request) {
        if (initial_stream)
            stats.streamInitialPrefetches++;
        else
            stats.streamPrefetches++;
    }
}

PacketPtr
DistantJoltPrefetcher::getPacket()
{
    if (queue.empty() || queue.front().ready > curTick())
        return nullptr;
    auto entry = queue.front();
    queue.pop_front();
    queuedAddresses.erase(entry.address);
    stats.issued++;
    prefetchStats.pfIssued++;
    return entry.packet;
}

Tick
DistantJoltPrefetcher::nextPrefetchReadyTime() const
{
    return queue.empty() ? MaxTick : queue.front().ready;
}

DistantJoltPrefetcher::Stats::Stats(statistics::Group *parent)
  : statistics::Group(parent),
    ADD_STAT(fetchControlInstructions, statistics::units::Count::get(),
             "Fetched control instructions that updated D-JOLT signatures"),
    ADD_STAT(cacheMisses, statistics::units::Count::get(),
             "Demand L1I misses learned by D-JOLT"),
    ADD_STAT(upperBitTableFull, statistics::units::Count::get(),
             "Misses rejected because the 31-entry upper-bit table was full"),
    ADD_STAT(missTableLearning, statistics::units::Count::get(),
             "D-JOLT main miss-table updates"),
    ADD_STAT(extraMissLearning, statistics::units::Count::get(),
             "D-JOLT extra miss-table updates"),
    ADD_STAT(signatureLookups, statistics::units::Count::get(),
             "D-JOLT signature table lookups"),
    ADD_STAT(signaturePrefetches, statistics::units::Count::get(),
             "D-JOLT signature-derived prefetch candidates"),
    ADD_STAT(streamInitialPrefetches, statistics::units::Count::get(),
             "D-JOLT fallback stream initial prefetches"),
    ADD_STAT(streamPrefetches, statistics::units::Count::get(),
             "D-JOLT fallback stream prefetches"),
    ADD_STAT(queueDrops, statistics::units::Count::get(),
             "D-JOLT candidates dropped by the request queue"),
    ADD_STAT(cacheSnoopDrops, statistics::units::Count::get(),
             "D-JOLT candidates already in L1I or its miss queue"),
    ADD_STAT(issued, statistics::units::Count::get(),
             "D-JOLT prefetch packets issued")
{
}

} // namespace prefetch
} // namespace gem5
