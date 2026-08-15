/* Copyright (c) 2026 */

#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_EMISSARY_LRU_RP_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_EMISSARY_LRU_RP_HH__

#include "base/random.hh"
#include "mem/cache/replacement_policies/lru_rp.hh"

namespace gem5
{
struct EmissaryLRURPParams;
namespace replacement_policy
{

class EmissaryLRU : public LRU
{
  protected:
    struct EmissaryReplData : LRUReplData {
        bool protectedLine = false;
    };

  public:
    typedef EmissaryLRURPParams Params;
    EmissaryLRU(const Params &p);
    using Base::reset;
    void invalidate(const std::shared_ptr<ReplacementData> &data) override;
    void reset(const std::shared_ptr<ReplacementData> &data,
               const PacketPtr pkt) override;
    ReplaceableEntry *getVictim(const ReplacementCandidates &candidates) const override;
    std::shared_ptr<ReplacementData> instantiateEntry() override;

  private:
    const unsigned protectedWays;
    const unsigned promotionProbability;
    const bool fecPrefetchSource;
    Random::RandomPtr rng;
    bool promote() const;
};

} // namespace replacement_policy
} // namespace gem5

#endif
