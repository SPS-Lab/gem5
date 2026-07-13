/*
 * Prototype LMHint prefetcher for PF-LLM generated load-PC hints.
 */

#ifndef __MEM_CACHE_PREFETCH_LMHINT_HH__
#define __MEM_CACHE_PREFETCH_LMHINT_HH__

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/statistics.hh"
#include "base/types.hh"
#include "mem/cache/prefetch/queued.hh"

namespace gem5
{

struct LMHintPrefetcherParams;

namespace prefetch
{

class LMHintPrefetcher : public Queued
{
  private:
    enum class PrefetcherSel : uint8_t
    {
        NoPrefetch = 0,
        StridePrefetcher = 1,
        TaggedPrefetcher = 2,
        IndirectMemoryPrefetcher = 3,
        SignaturePathPrefetcherV2 = 4,
        AMPMPrefetcher = 5,
        DCPTPrefetcher = 6,
        SBOOEPrefetcher = 7,
        SmsPrefetcher = 8,
        BOPPrefetcher = 9,
    };

    struct Hint
    {
        PrefetcherSel pfSel = PrefetcherSel::NoPrefetch;
        unsigned degree = 1;
        PrefetcherSel filter = PrefetcherSel::NoPrefetch;
        uint16_t raw = 0;
    };

    struct PcState
    {
        Addr lastAddr = 0;
        int64_t lastStride = 0;
        bool valid = false;
    };

    struct LMHintStats : public statistics::Group
    {
        LMHintStats(statistics::Group *parent);

        statistics::Scalar tableEntries;
        statistics::Scalar tableHits;
        statistics::Scalar tableMisses;
        statistics::Scalar noPc;
        statistics::Scalar noPrefetchHints;
        statistics::Scalar generated;
    } lmHintStats;

    const std::string hintTableFile;
    const Addr fallbackDistance;

    uint64_t textBase = 0;
    bool loadAttempted = false;
    std::unordered_map<Addr, Hint> hints;
    std::unordered_map<Addr, PcState> pcStates;

    static Hint decodeHint(uint16_t raw);
    static const char *prefetcherName(PrefetcherSel sel);

    std::string autoDetectHintTableFile() const;
    void loadHintTable();
    bool loadRawHintTable(const std::string &path);
    bool loadElfHintTable(const std::string &path);
    bool parseHintPayload(const std::vector<uint8_t> &payload,
                          const std::string &source);

    void generateNextLine(const PrefetchInfo &pfi,
                          std::vector<AddrPriority> &addresses,
                          unsigned degree) const;
    void generateStride(Addr pc, const PrefetchInfo &pfi,
                        std::vector<AddrPriority> &addresses,
                        unsigned degree);

  public:
    LMHintPrefetcher(const LMHintPrefetcherParams &p);
    ~LMHintPrefetcher() = default;

    void startup() override;

    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses,
                           const CacheAccessor &cache) override;
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_LMHINT_HH__
