/* Copyright (c) 2026 */

#include "mem/cache/prefetch/pdip.hh"

#include <algorithm>

#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/random.hh"
#include "cpu/o3/dyn_inst.hh"
#include "debug/HWPrefetch.hh"
#include "params/PriorityDirectedPrefetcher.hh"

namespace gem5
{
namespace prefetch
{

PriorityDirectedPrefetcher::PriorityDirectedPrefetcher(const Params &p)
    : Base(p), cpu(p.cpu), cache(nullptr), enabled(p.enabled),
      tableSets(p.table_sets), tableAssoc(p.table_assoc), tagBits(p.tag_bits),
      targetAddressBits(p.target_address_bits),
      targetsPerEntry(p.targets_per_entry), targetMaskBits(p.target_mask_bits),
      trainingProbability(p.training_probability), minFreeMSHRs(p.min_free_mshrs),
      highCost(cyclesToTicks(Cycles(p.high_cost_cycles))),
      requireBackendStall(p.require_backend_stall),
      ignoreReturns(p.ignore_returns), markReqAsPrefetch(p.mark_req_as_prefetch),
      squashPrefetches(p.squash_prefetches), cacheSnoop(p.cache_snoop),
      latency(cyclesToTicks(p.latency)), pfqSize(p.pfq_size), tqSize(p.tq_size),
      rng(Random::genRandom()), table(tableSets, std::vector<Entry>(tableAssoc)),
      stats(this)
{
    fatal_if(tableSets == 0 || tableAssoc == 0 || targetsPerEntry == 0,
             "PDIP table dimensions must be non-zero");
    fatal_if(tagBits == 0 || tagBits > 63, "PDIP tag_bits must be in [1, 63]");
    fatal_if(targetAddressBits == 0 || targetAddressBits > 63,
             "PDIP target_address_bits must be in [1, 63]");
    fatal_if(targetMaskBits == 0 || targetMaskBits > 63,
             "PDIP target_mask_bits must be in [1, 63]");
    fatal_if((tableSets & (tableSets - 1)) != 0,
             "PDIP table_sets must be a power of two");
    resteerTriggers.fill(MaxAddr);
    resteerWindows.fill(0);
    lastTakenBranches.fill(MaxAddr);
}

Tick
PriorityDirectedPrefetcher::nextPrefetchReadyTime() const
{
    return pfq.empty() ? MaxTick : pfq.front().readyTime;
}

size_t
PriorityDirectedPrefetcher::setIndex(Addr trigger) const
{
    return (trigger >> lBlkSize) & (tableSets - 1);
}

Addr
PriorityDirectedPrefetcher::tag(Addr trigger) const
{
    // Index bits and tag bits must not overlap. The previous encoding used
    // the low block-address bits for both, aliasing nearly every large binary.
    const Addr raw = trigger >> (lBlkSize + floorLog2(tableSets));
    return raw & ((Addr(1) << tagBits) - 1);
}

PriorityDirectedPrefetcher::Entry *
PriorityDirectedPrefetcher::find(Addr trigger)
{
    for (auto &entry : table[setIndex(trigger)]) {
        if (entry.valid && entry.tag == tag(trigger)) {
            entry.nru = false;
            return &entry;
        }
    }
    return nullptr;
}

const PriorityDirectedPrefetcher::Entry *
PriorityDirectedPrefetcher::find(Addr trigger) const
{
    for (const auto &entry : table[setIndex(trigger)]) {
        if (entry.valid && entry.tag == tag(trigger))
            return &entry;
    }
    return nullptr;
}

bool
PriorityDirectedPrefetcher::sample(unsigned probability)
{
    return probability == 100 ||
           (probability != 0 && rng->random<unsigned>(0, 99) < probability);
}

void
PriorityDirectedPrefetcher::train(Addr trigger, Addr target)
{
    if (!sample(trainingProbability) || trigger == target)
        return;
    Entry *entry = find(trigger);
    if (!entry) {
        auto &set = table[setIndex(trigger)];
        auto victim = std::find_if(set.begin(), set.end(),
            [](const Entry &candidate) { return !candidate.valid; });
        if (victim == set.end()) {
            victim = std::find_if(set.begin(), set.end(),
                [](const Entry &candidate) { return candidate.nru; });
            if (victim == set.end()) {
                for (auto &candidate : set)
                    candidate.nru = true;
                victim = set.begin();
            }
            stats.tableReplacements++;
        }
        entry = &*victim;
        entry->valid = true;
        entry->tag = tag(trigger);
        entry->nru = false;
        entry->targets.clear();
        stats.tableInsertions++;
    }
    entry->nru = false;
    for (auto &known : entry->targets) {
        if (target == known.address) {
            stats.trainingEvents++;
            return;
        }

        if (target > known.address &&
            target <= known.address + targetMaskBits * blkSize) {
            const unsigned offset = (target - known.address) / blkSize;
            known.mask |= Addr(1) << (offset - 1);
            stats.trainingEvents++;
            return;
        }

        // Keep the target address as the lowest line in the compact range.
        // This also lets a later-observed earlier line merge into a target
        // already stored in the table.
        if (target < known.address &&
            known.address - target <= targetMaskBits * blkSize) {
            const unsigned shift = (known.address - target) / blkSize;
            known.mask = (known.mask << shift) &
                ((Addr(1) << targetMaskBits) - 1);
            known.address = target;
            stats.trainingEvents++;
            return;
        }
    }
    if (entry->targets.size() == targetsPerEntry)
        entry->targets.erase(entry->targets.begin());
    entry->targets.push_back({target, 0});
    stats.trainingEvents++;
}

void
PriorityDirectedPrefetcher::enqueue(Addr address, ThreadID tid, o3::FTSeqNum ftn)
{
    if (pfq.size() >= pfqSize) {
        stats.droppedQueue++;
        return;
    }
    if (std::any_of(pfq.begin(), pfq.end(), [address](const auto &r) {
            return r.addr == address;
        }))
        return;
    if (cacheSnoop && cache &&
        (cache->inCache(address, false) || cache->inMissQueue(address, false))) {
        stats.droppedCacheSnoop++;
        return;
    }
    pfq.emplace_back(*this, address, tid, ftn);
    pfq.back().createPkt();
    pfq.back().readyTime = curTick() + latency;
}

void
PriorityDirectedPrefetcher::issueTargets(Addr trigger, ThreadID tid,
                                         o3::FTSeqNum ftn)
{
    stats.tableLookups++;
    Entry *entry = find(trigger);
    if (!entry) {
        stats.predictorMisses++;
        return;
    }
    stats.predictorHits++;
    if (cache && cache->numFreeMSHRs() <= int(minFreeMSHRs)) {
        stats.droppedMSHR++;
        return;
    }
    for (const auto &target : entry->targets) {
        stats.candidates++;
        enqueue(target.address, tid, ftn);
        for (unsigned bit = 0; bit < targetMaskBits; ++bit) {
            if (target.mask & (Addr(1) << bit)) {
                stats.candidates++;
                enqueue(target.address + (bit + 1) * blkSize, tid, ftn);
            }
        }
    }
}

void
PriorityDirectedPrefetcher::notifyFTQInsert(const o3::FetchTargetPtr &ft)
{
    if (!enabled)
        return;
    const Addr start = blockAddress(ft->startAddress());
    const Addr end = blockAddress(ft->endAddress());
    const ThreadID tid = ft->getTid();
    Addr trigger = MaxAddr;
    bool resteer_path = false;
    if (resteerWindows[tid] != 0) {
        trigger = resteerTriggers[tid];
        --resteerWindows[tid];
        resteer_path = true;
    } else {
        trigger = lastTakenBranches[tid];
    }
    FTQState state;
    state.start = start;
    state.end = end;
    state.trigger = trigger;
    state.tid = tid;
    state.resteerPath = resteer_path;
    ftqs[ft->ftNum()] = std::move(state);
    stats.ftqInsertions++;
    // The PDIP controller receives the BTB-hit branch block. When no BTB
    // branch ends this fetch target, its NIP is the target's start block.
    const bool branch_lookup = ft->isExitBranch(ft->endAddress());
    const Addr lookup = blockAddress(branch_lookup ? ft->endAddress() :
                                     ft->startAddress());
    if (branch_lookup)
        stats.branchBlockLookups++;
    else
        stats.nipLookups++;
    issueTargets(lookup, tid, ft->ftNum());
}

void
PriorityDirectedPrefetcher::notifyFTQRemove(const o3::FetchTargetPtr &ft)
{
    auto it = ftqs.find(ft->ftNum());
    if (it == ftqs.end())
        return;
    recentFtqs.push_back(std::move(it->second));
    ftqs.erase(it);
    retireOldFTQs();
}

void
PriorityDirectedPrefetcher::finalizeFEC(FTQState &state, Addr retired_pc)
{
    const Addr retired_block = blockAddress(retired_pc);
    for (auto &miss : state.misses) {
        // A demand miss becomes an FEC only after an instruction from that
        // exact cache line retires on the correct path.
        if (miss.finalized || miss.pc != retired_block)
            continue;

        miss.finalized = true;
        stats.finalizedFECs++;
        stats.retiredMisses++;
        const bool high_cost = curTick() - miss.missTick >= highCost;
        if (!high_cost) {
            stats.filteredLowCost++;
            continue;
        }
        if (requireBackendStall && !miss.backendStalled) {
            stats.filteredNoBackendStall++;
            continue;
        }
        if (state.trigger == MaxAddr)
            continue;
        stats.criticalBlocks++;
        if (state.resteerPath)
            stats.resteerTriggers++;
        else
            stats.lastTakenTriggers++;
        if (miss.address >= (Addr(1) << targetAddressBits)) {
            stats.targetAddressOverflows++;
            continue;
        }
        train(state.trigger, miss.address);
    }
}

void
PriorityDirectedPrefetcher::notifyFTQSquash(const o3::FetchTargetPtr &ft)
{
    ftqs.erase(ft->ftNum());
    if (!squashPrefetches)
        return;
    for (auto it = pfq.begin(); it != pfq.end();) {
        if (it->ftn == ft->ftNum()) {
            if (it->pkt)
                delete it->pkt;
            it = pfq.erase(it);
            stats.squashed++;
        } else {
            ++it;
        }
    }
}

void
PriorityDirectedPrefetcher::notifyCacheMiss(const CacheAccessProbeArg &arg)
{
    if (!enabled || !arg.pkt->req->isInstFetch() || arg.pkt->req->isPrefetch())
        return;

    stats.instCacheMisses++;
    // FTQ ranges are virtual instruction PCs. The packet address has already
    // been translated to physical by the time the L1I emits its miss probe,
    // so use the request PC to compare addresses in the same domain.
    const Addr block = blockAddress(arg.pkt->req->getPC());
    const Addr paddr = blockAddress(arg.pkt->req->getPaddr());
    FTQState *matched_state = nullptr;
    for (auto &entry : ftqs) {
        auto &state = entry.second;
        if (block >= state.start && block <= state.end) {
            matched_state = &state;
            break;
        }
    }
    if (matched_state) {
        const auto known = std::find_if(matched_state->misses.begin(),
            matched_state->misses.end(), [block](const MissState &miss) {
                return miss.pc == block;
            });
        if (known == matched_state->misses.end())
            matched_state->misses.push_back({block, paddr, curTick()});
        stats.missesMatchedFTQ++;
        return;
    }

    // The FTQ entry may have been popped before this asynchronous cache miss
    // reaches the probe. Prefer the newest matching consumed entry.
    for (auto state = recentFtqs.rbegin(); state != recentFtqs.rend(); ++state) {
        if (block >= state->start && block <= state->end) {
            const auto known = std::find_if(state->misses.begin(),
                state->misses.end(), [block](const MissState &miss) {
                    return miss.pc == block;
                });
            if (known == state->misses.end())
                state->misses.push_back({block, paddr, curTick()});
            stats.missesMatchedFTQ++;
            stats.missesMatchedRecentFTQ++;
            return;
        }
    }
    stats.missesUnmatchedFTQ++;
}

PriorityDirectedPrefetcher::FTQState *
PriorityDirectedPrefetcher::findFTQ(Addr pc, ThreadID tid)
{
    const Addr block = blockAddress(pc);
    for (auto &entry : ftqs) {
        auto &state = entry.second;
        if (state.tid == tid && block >= state.start && block <= state.end)
            return &state;
    }
    for (auto it = recentFtqs.rbegin(); it != recentFtqs.rend(); ++it) {
        if (it->tid == tid && block >= it->start && block <= it->end)
            return &*it;
    }
    return nullptr;
}

void
PriorityDirectedPrefetcher::retireOldFTQs()
{
    while (recentFtqs.size() > RecentFTQWindow) {
        recentFtqs.pop_front();
    }
}

void
PriorityDirectedPrefetcher::notifyMispredict(const o3::DynInstPtr &inst)
{
    if (!enabled || !inst || (ignoreReturns && inst->isReturn()))
        return;
    const ThreadID tid = inst->threadNumber;
    resteerTriggers[tid] = blockAddress(inst->pcState().instAddr());
    // The default O3 FDIP configuration has a 24-entry FTQ. Keep the trigger
    // only over this refill window; later misses use the last-taken fallback.
    resteerWindows[tid] = 24;
}

void
PriorityDirectedPrefetcher::notifyCommit(const o3::DynInstPtr &inst)
{
    if (!enabled || !inst)
        return;
    const ThreadID tid = inst->threadNumber;
    if (inst->isControl() && !(ignoreReturns && inst->isReturn()) &&
        (inst->isUncondCtrl() || inst->readPredTaken() || inst->mispredicted()))
        lastTakenBranches[tid] = blockAddress(inst->pcState().instAddr());
    if (auto *state = findFTQ(inst->pcState().instAddr(), tid)) {
        finalizeFEC(*state, inst->pcState().instAddr());
    }
}

void
PriorityDirectedPrefetcher::notifyBackendStall(ThreadID tid)
{
    if (!enabled)
        return;

    MissState *candidate = nullptr;
    for (auto &entry : ftqs) {
        auto &state = entry.second;
        if (state.tid != tid)
            continue;
        for (auto &miss : state.misses) {
            if (!miss.finalized &&
                (!candidate || miss.missTick > candidate->missTick))
                candidate = &miss;
        }
    }
    for (auto it = recentFtqs.rbegin(); it != recentFtqs.rend(); ++it) {
        if (it->tid != tid)
            continue;
        for (auto &miss : it->misses) {
            if (!miss.finalized &&
                (!candidate || miss.missTick > candidate->missTick))
                candidate = &miss;
        }
    }
    if (candidate)
        candidate->backendStalled = true;
}

PacketPtr
PriorityDirectedPrefetcher::getPacket()
{
    if (pfq.empty())
        return nullptr;
    PacketPtr packet = pfq.front().pkt;
    pfq.pop_front();
    stats.issued++;
    prefetchStats.pfIssued++;
    return packet;
}

PriorityDirectedPrefetcher::PrefetchRequest::PrefetchRequest(
    PriorityDirectedPrefetcher &_owner, Addr _addr, ThreadID tid,
    o3::FTSeqNum _ftn)
    : addr(_addr), ftn(_ftn)
{
    req = std::make_shared<Request>(addr, _owner.blkSize, Request::INST_FETCH,
        _owner.requestorId);
    if (_owner.markReqAsPrefetch)
        req->setFlags(Request::PREFETCH);
    req->setFlags(Request::PDIP_FEC);
}

void
PriorityDirectedPrefetcher::PrefetchRequest::createPkt()
{
    req->taskId(context_switch_task_id::Prefetcher);
    pkt = new Packet(req, MemCmd::HardPFReq);
    pkt->allocate();
}

void
PriorityDirectedPrefetcher::regProbeListeners()
{
    Base::regProbeListeners();
    if (!cpu) {
        warn("PriorityDirectedPrefetcher: no CPU was configured\n");
        return;
    }
    using FTListener = ProbeListenerArgFunc<o3::FetchTargetPtr>;
    listeners.push_back(cpu->getProbeManager()->connect<FTListener>("FTQInsert",
        [this](const auto &ft) { notifyFTQInsert(ft); }));
    listeners.push_back(cpu->getProbeManager()->connect<FTListener>("FTQRemove",
        [this](const auto &ft) { notifyFTQRemove(ft); }));
    listeners.push_back(cpu->getProbeManager()->connect<FTListener>("FTQSquash",
        [this](const auto &ft) { notifyFTQSquash(ft); }));
    using InstListener = ProbeListenerArgFunc<o3::DynInstPtr>;
    listeners.push_back(cpu->getProbeManager()->connect<InstListener>("Mispredict",
        [this](const auto &inst) { notifyMispredict(inst); }));
    listeners.push_back(cpu->getProbeManager()->connect<InstListener>("Commit",
        [this](const auto &inst) { notifyCommit(inst); }));
    using BackendStallListener = ProbeListenerArgFunc<ThreadID>;
    listeners.push_back(cpu->getProbeManager()->connect<BackendStallListener>(
        "BackendStall", [this](ThreadID tid) { notifyBackendStall(tid); }));
    if (cache) {
        using CacheListener = ProbeListenerArgFunc<CacheAccessProbeArg>;
        listeners.push_back(cache->getProbeManager()->connect<CacheListener>("Miss",
            [this](const auto &arg) { notifyCacheMiss(arg); }));
    }
}

PriorityDirectedPrefetcher::Stats::Stats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(ftqInsertions, statistics::units::Count::get(), "FTQ insertions"),
      ADD_STAT(tableLookups, statistics::units::Count::get(), "PDIP table lookups"),
      ADD_STAT(branchBlockLookups, statistics::units::Count::get(),
               "PDIP lookups keyed by a BTB branch block"),
      ADD_STAT(nipLookups, statistics::units::Count::get(),
               "PDIP lookups keyed by a BTB-miss NIP block"),
      ADD_STAT(tableInsertions, statistics::units::Count::get(), "PDIP table allocations"),
      ADD_STAT(tableReplacements, statistics::units::Count::get(), "PDIP table replacements"),
      ADD_STAT(instCacheMisses, statistics::units::Count::get(),
               "Observed demand instruction-cache misses"),
      ADD_STAT(missesMatchedFTQ, statistics::units::Count::get(),
               "Instruction-cache misses matched to an FTQ entry"),
      ADD_STAT(missesMatchedRecentFTQ, statistics::units::Count::get(),
               "Instruction-cache misses matched after FTQ consumption"),
      ADD_STAT(missesUnmatchedFTQ, statistics::units::Count::get(),
               "Instruction-cache misses not matched to a live FTQ entry"),
      ADD_STAT(finalizedFECs, statistics::units::Count::get(),
               "Retired cache-line misses finalized for PDIP training"),
      ADD_STAT(retiredMisses, statistics::units::Count::get(),
               "Instruction-cache misses whose line retired"),
      ADD_STAT(filteredLowCost, statistics::units::Count::get(),
               "Retired misses below the FEC cost threshold"),
      ADD_STAT(filteredNoBackendStall, statistics::units::Count::get(),
               "High-cost misses without an issue-queue-empty observation"),
      ADD_STAT(resteerTriggers, statistics::units::Count::get(),
               "FEC blocks trained from a mispredict trigger"),
      ADD_STAT(lastTakenTriggers, statistics::units::Count::get(),
               "FEC blocks trained from a last-taken trigger"),
      ADD_STAT(targetAddressOverflows, statistics::units::Count::get(),
               "FEC targets outside the configured physical-address width"),
      ADD_STAT(criticalBlocks, statistics::units::Count::get(), "Trained FEC blocks"),
      ADD_STAT(trainingEvents, statistics::units::Count::get(), "Predictor updates"),
      ADD_STAT(predictorHits, statistics::units::Count::get(), "Predictor hits"),
      ADD_STAT(predictorMisses, statistics::units::Count::get(), "Predictor misses"),
      ADD_STAT(candidates, statistics::units::Count::get(), "Candidate lines"),
      ADD_STAT(issued, statistics::units::Count::get(), "Issued PDIP prefetches"),
      ADD_STAT(squashed, statistics::units::Count::get(), "Canceled PDIP requests"),
      ADD_STAT(droppedCacheSnoop, statistics::units::Count::get(),
               "Candidates already present in the cache or an MSHR"),
      ADD_STAT(droppedMSHR, statistics::units::Count::get(), "Candidates held for MSHRs"),
      ADD_STAT(droppedQueue, statistics::units::Count::get(), "Queue drops")
{}

} // namespace prefetch
} // namespace gem5
