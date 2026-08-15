/* Copyright (c) 2026 */

#include "mem/cache/replacement_policies/emissary_lru_rp.hh"

#include "params/EmissaryLRURP.hh"

namespace gem5
{
namespace replacement_policy
{

EmissaryLRU::EmissaryLRU(const Params &p)
    : LRU(p), protectedWays(p.protected_ways),
      promotionProbability(p.promotion_probability),
      fecPrefetchSource(p.enable_fec_prefetch_source), rng(Random::genRandom())
{}

bool
EmissaryLRU::promote() const
{
    return promotionProbability == 100 ||
           (promotionProbability != 0 &&
            rng->random<unsigned>(0, 99) < promotionProbability);
}

void
EmissaryLRU::invalidate(const std::shared_ptr<ReplacementData> &data)
{
    LRU::invalidate(data);
    std::static_pointer_cast<EmissaryReplData>(data)->protectedLine = false;
}

void
EmissaryLRU::reset(const std::shared_ptr<ReplacementData> &data,
                   const PacketPtr pkt)
{
    LRU::reset(data);
    auto emissary = std::static_pointer_cast<EmissaryReplData>(data);
    emissary->protectedLine = fecPrefetchSource && pkt &&
        pkt->req->isPDIPFEC() && promote();
}

ReplaceableEntry *
EmissaryLRU::getVictim(const ReplacementCandidates &candidates) const
{
    assert(!candidates.empty());
    unsigned protectedCount = 0;
    for (const auto candidate : candidates) {
        protectedCount += std::static_pointer_cast<EmissaryReplData>(
            candidate->replacementData)->protectedLine;
    }
    const bool preferUnprotected = protectedCount < protectedWays &&
        protectedCount != candidates.size();
    ReplaceableEntry *victim = nullptr;
    for (const auto candidate : candidates) {
        const auto data = std::static_pointer_cast<EmissaryReplData>(
            candidate->replacementData);
        if (preferUnprotected && data->protectedLine)
            continue;
        if (!victim || data->lastTouchTick <
                std::static_pointer_cast<EmissaryReplData>(
                    victim->replacementData)->lastTouchTick)
            victim = candidate;
    }
    return victim;
}

std::shared_ptr<ReplacementData>
EmissaryLRU::instantiateEntry()
{
    return std::make_shared<EmissaryReplData>();
}

} // namespace replacement_policy
} // namespace gem5
