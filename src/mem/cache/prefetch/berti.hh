/*
 * Adapted from the Berti prefetcher in OpenXiangShan/GEM5
 * (src/mem/cache/prefetch/berti.{cc,hh}, BSD-3-Clause).
 *
 * Differences from that version:
 *  - usable on its own, without XSCompositePrefetcher;
 *  - timeliness uses the measured fill latency of the demand or prefetch;
 *  - a new PC does not inherit the deltas of the entry it replaces.
 */

/**
 * @file
 * Describes the Berti prefetcher: per-PC local deltas filtered by timeliness.
 */

#ifndef __MEM_CACHE_PREFETCH_BERTI_HH__
#define __MEM_CACHE_PREFETCH_BERTI_HH__

#include <cstdint>
#include <deque>
#include <list>
#include <unordered_map>
#include <vector>

#include "base/cache/associative_cache.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "mem/cache/prefetch/queued.hh"
#include "mem/cache/tags/tagged_entry.hh"
#include "mem/packet.hh"

namespace gem5
{

struct BertiPrefetcherParams;

namespace prefetch
{

class Berti : public Queued
{
  protected:
    struct HistoryInfo
    {
        Addr vAddr;
        Cycles timestamp;
    };

    enum DeltaStatus { L1_PREF, L2_PREF, NO_PREF };

    struct DeltaInfo
    {
        uint8_t coverageCounter = 0;
        int64_t delta = 0;
        DeltaStatus status = NO_PREF;
    };

    /** History and table of deltas of one PC. */
    struct HistoryTableEntry : public TaggedEntry
    {
        HistoryTableEntry(int num_deltas, TagExtractor ext);

        /** Forget everything learned from the previous PC. */
        void resetForNewPC(Addr pc_key, bool is_secure);
        void resetConfidence();
        void updateStatus();

        std::vector<DeltaInfo> deltas;
        uint8_t counter = 0;
        DeltaInfo bestDelta;

        /** Second chance on replacement; set when the history grows. */
        bool hysteresis = false;
        /** Kept to re-insert the entry, as findVictim invalidates its tag. */
        Addr pcKey = 0;
        bool secure = false;
        /** FIFO of demand addresses. */
        std::list<HistoryInfo> history;
    };

    const unsigned maxAddrListSize;
    const unsigned maxDeltaFound;
    const unsigned pfLatencyEntries;
    const bool aggressivePF;
    const bool useByteAddr;

    AssociativeCache<HistoryTableEntry> historyTable;

    /** Fill latency of prefetched blocks not yet hit, by physical block
     * address. Bounded FIFO, as this is only a training hint. */
    std::unordered_map<Addr, Cycles> pfLatency;
    std::deque<Addr> pfLatencyOrder;

    struct BertiStats : public statistics::Group
    {
        BertiStats(statistics::Group *parent);

        statistics::Scalar trainOnHit;
        statistics::Scalar trainOnMiss;
        statistics::Scalar notifySkippedCond1;
        statistics::Scalar notifySkippedNoEntry;
        statistics::Scalar entryEvicts;
        statistics::Scalar pfLatencyRecorded;
        statistics::Scalar hitSkippedNoLatency;
        statistics::Scalar deltasUntimely;
    } statsBerti;

    static Addr pcHash(Addr pc) { return pc >> 1; }

    /** Address as used for training: byte address or block index. */
    Addr trainingAddr(Addr addr) const
    {
        return useByteAddr ? addr : blockIndex(addr);
    }

    /**
     * Add a demand address to the history of its PC.
     * @return The entry if the prefetch can use it, nullptr otherwise.
     */
    HistoryTableEntry *updateHistoryTable(const PrefetchInfo &pfi);

    /**
     * Train on an access: history addresses old enough for a prefetch issued
     * there to have finished by demand_cycle yield timely deltas.
     */
    void searchTimelyDeltas(HistoryTableEntry &entry, Cycles latency,
                            Cycles demand_cycle, Addr trigger_addr);

    void sendPrefetch(const PrefetchInfo &pfi, int64_t delta,
                      std::vector<AddrPriority> &addresses) const;

  public:
    Berti(const BertiPrefetcherParams &p);

    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses,
                           const CacheAccessor &cache) override;

    void notifyFill(const CacheAccessProbeArg &acc) override;
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_BERTI_HH__
