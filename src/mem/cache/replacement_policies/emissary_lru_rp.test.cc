/* Copyright (c) 2026 */

#include <gtest/gtest.h>

#include "mem/cache/replacement_policies/emissary_lru_rp.hh"
#include "mem/packet.hh"
#include "params/EmissaryLRURP.hh"

namespace
{

class EmissaryLRUTest : public ::testing::Test
{
  protected:
    gem5::EventQueue eventQueue{"EmissaryLRUTest Queue"};
    gem5::EmissaryLRURPParams params;
    std::shared_ptr<gem5::replacement_policy::EmissaryLRU> policy;

    void SetUp() override
    {
        params.eventq_index = 0;
        params.protected_ways = 1;
        params.promotion_probability = 100;
        params.enable_fec_prefetch_source = true;
        policy = std::make_shared<gem5::replacement_policy::EmissaryLRU>(
            params);
        gem5::curEventQueue(&eventQueue);
    }

    gem5::PacketPtr fecPacket()
    {
        auto request = std::make_shared<gem5::Request>(
            0x1000, 64, gem5::Request::INST_FETCH | gem5::Request::PREFETCH,
            0);
        request->setFlags(gem5::Request::PDIP_FEC);
        return new gem5::Packet(request, gem5::MemCmd::HardPFReq);
    }
};

TEST_F(EmissaryLRUTest, SingleCandidateIsAlwaysVictim)
{
    gem5::ReplaceableEntry entry;
    entry.replacementData = policy->instantiateEntry();
    gem5::ReplacementCandidates candidates{&entry};
    EXPECT_EQ(policy->getVictim(candidates), &entry);
}

TEST_F(EmissaryLRUTest, ProtectsFecLineWhileQuotaIsAvailable)
{
    gem5::ReplaceableEntry protectedEntry;
    gem5::ReplaceableEntry unprotectedEntry;
    protectedEntry.replacementData = policy->instantiateEntry();
    unprotectedEntry.replacementData = policy->instantiateEntry();
    gem5::ReplacementCandidates candidates{&protectedEntry, &unprotectedEntry};

    policy->reset(unprotectedEntry.replacementData);
    eventQueue.setCurTick(gem5::curTick() + 1);
    auto packet = fecPacket();
    policy->reset(protectedEntry.replacementData, packet);
    delete packet;

    EXPECT_EQ(policy->getVictim(candidates), &unprotectedEntry);
}

TEST_F(EmissaryLRUTest, FallsBackToLruWhenProtectionQuotaIsFull)
{
    gem5::ReplaceableEntry oldProtected;
    gem5::ReplaceableEntry newUnprotected;
    gem5::ReplaceableEntry secondProtected;
    oldProtected.replacementData = policy->instantiateEntry();
    newUnprotected.replacementData = policy->instantiateEntry();
    secondProtected.replacementData = policy->instantiateEntry();
    gem5::ReplacementCandidates candidates{
        &oldProtected, &newUnprotected, &secondProtected};

    auto packet = fecPacket();
    policy->reset(oldProtected.replacementData, packet);
    delete packet;
    eventQueue.setCurTick(gem5::curTick() + 1);
    policy->reset(newUnprotected.replacementData);
    eventQueue.setCurTick(gem5::curTick() + 1);
    packet = fecPacket();
    policy->reset(secondProtected.replacementData, packet);
    delete packet;

    // Two protected lines exceed the one-line quota, so ordinary LRU applies.
    EXPECT_EQ(policy->getVictim(candidates), &oldProtected);
}

TEST_F(EmissaryLRUTest, InvalidateReleasesProtection)
{
    gem5::ReplaceableEntry protectedEntry;
    gem5::ReplaceableEntry unprotectedEntry;
    protectedEntry.replacementData = policy->instantiateEntry();
    unprotectedEntry.replacementData = policy->instantiateEntry();
    gem5::ReplacementCandidates candidates{&protectedEntry, &unprotectedEntry};

    auto packet = fecPacket();
    policy->reset(protectedEntry.replacementData, packet);
    delete packet;
    eventQueue.setCurTick(gem5::curTick() + 1);
    policy->reset(unprotectedEntry.replacementData);
    policy->invalidate(protectedEntry.replacementData);

    EXPECT_EQ(policy->getVictim(candidates), &protectedEntry);
}

} // anonymous namespace
