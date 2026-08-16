/*
 * Copyright (c) 2026
 * All rights reserved.
 */

#ifndef __MEM_CACHE_PREFETCH_D_JOLT_HH__
#define __MEM_CACHE_PREFETCH_D_JOLT_HH__

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cpu/base.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "mem/cache/prefetch/base.hh"

namespace gem5
{

struct DistantJoltPrefetcherParams;

namespace prefetch
{

/** IPC-1 D-JOLT instruction prefetcher, used as an FDIP companion. */
class DistantJoltPrefetcher : public Base
{
  public:
    using Params = DistantJoltPrefetcherParams;

    DistantJoltPrefetcher(const Params &p);
    ~DistantJoltPrefetcher() override;

    void regProbeListeners() override;
    void notify(const CacheAccessProbeArg &acc,
                const PrefetchInfo &pfi) override;
    PacketPtr getPacket() override;
    Tick nextPrefetchReadyTime() const override;

  private:
    static constexpr unsigned SignatureBits = 23;
    static constexpr uint64_t UpperBitMask = 0xffffffffff000000ULL;

    struct CompressedAddress {
        unsigned upper = 0;
        uint64_t lower = 0;
        bool valid() const { return upper != 0; }
    };

    struct MissInfo {
        CompressedAddress base;
        uint8_t bits = 0;
        bool add(const CompressedAddress &address);
        std::vector<CompressedAddress> addresses() const;
    };

    class UpperBitTable {
      public:
        std::pair<bool, CompressedAddress> compress(Addr addr,
                                                     unsigned line_bits);
        Addr decompress(const CompressedAddress &address,
                        unsigned line_bits) const;

      private:
        struct Entry { bool valid = false; uint64_t upper = 0; };
        std::array<Entry, 31> entries{};
    };

    class SignatureGenerator {
      public:
        explicit SignatureGenerator(unsigned history_length);
        uint32_t onCall(Addr pc);
        uint32_t onReturn();

      private:
        uint32_t makeSignature() const;
        std::vector<uint32_t> history;
        unsigned head = 0;
        uint64_t returns = 0;
    };

    class SignatureQueue {
      public:
        explicit SignatureQueue(unsigned distance);
        void push(uint32_t signature);
        uint32_t delayed() const;

      private:
        std::vector<uint32_t> entries;
        unsigned head = 0;
    };

    class MissTable {
      public:
        MissTable(unsigned sets, unsigned ways);
        bool insert(uint32_t signature, const CompressedAddress &address);
        void touch(uint32_t signature);
        std::vector<CompressedAddress> lookup(uint32_t signature) const;

      private:
        struct Entry {
            bool valid = false;
            uint32_t signature = 0;
            std::array<MissInfo, 2> vectors{};
            uint64_t touch = 0;
        };
        unsigned sets;
        unsigned ways;
        mutable uint64_t sequence = 0;
        std::vector<Entry> entries;
        Entry *find(uint32_t signature);
        const Entry *find(uint32_t signature) const;
        Entry &allocate(uint32_t signature);
    };

    class StreamPrefetcher {
      public:
        explicit StreamPrefetcher(DistantJoltPrefetcher &owner);
        void access(Addr line, bool miss, bool prefetch_hit,
                    const CacheAccessor &cache);

      private:
        struct Training { bool valid = false; Addr start = 0; unsigned count = 0; uint64_t touch = 0; };
        struct Monitoring { bool valid = false; Addr start = 0; uint64_t touch = 0; };
        DistantJoltPrefetcher &owner;
        std::array<Training, 16> training{};
        std::array<Monitoring, 16> monitoring{};
        uint64_t sequence = 0;
        void allocateTraining(Addr line);
        void allocateMonitoring(Addr line);
    };

    struct QueuedPacket {
        PacketPtr packet = nullptr;
        Addr address = 0;
        Tick ready = MaxTick;
    };

    struct Stats : public statistics::Group {
        Stats(statistics::Group *parent);
        statistics::Scalar fetchControlInstructions;
        statistics::Scalar cacheMisses;
        statistics::Scalar upperBitTableFull;
        statistics::Scalar missTableLearning;
        statistics::Scalar extraMissLearning;
        statistics::Scalar signatureLookups;
        statistics::Scalar signaturePrefetches;
        statistics::Scalar streamInitialPrefetches;
        statistics::Scalar streamPrefetches;
        statistics::Scalar queueDrops;
        statistics::Scalar cacheSnoopDrops;
        statistics::Scalar issued;
    } stats;

    BaseCPU *cpu;
    const unsigned queueSize;
    const Tick latency;
    const bool cacheSnoop;
    std::vector<ProbeListenerPtr<>> probeListeners;
    std::deque<QueuedPacket> queue;
    std::unordered_set<Addr> queuedAddresses;

    SignatureGenerator shortGenerator;
    SignatureGenerator longGenerator;
    SignatureQueue shortHistory;
    SignatureQueue longHistory;
    MissTable shortMissTable;
    MissTable longMissTable;
    MissTable extraMissTable;
    UpperBitTable upperBits;
    StreamPrefetcher stream;

    void onFetch(const o3::DynInstPtr &inst);
    void accessCache(Addr addr, bool miss, bool prefetch_hit,
                     const CacheAccessor &cache);
    void learn(MissTable &table, uint32_t signature,
               const CompressedAddress &address);
    void prefetchFor(const MissTable &table, uint32_t signature,
                     const CacheAccessor *cache);
    void enqueue(Addr address, const CacheAccessor *cache,
                 bool stream_request, bool initial_stream);
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_D_JOLT_HH__
