/**
 * @file
 * Describes Vector Berti: Berti that learns per vle instruction. History and
 * deltas use the vector base address, and a prefetch covers every cache line
 * of the target vl*eew range. Other loads are handled by plain Berti.
 */

#ifndef __MEM_CACHE_PREFETCH_VECTOR_BERTI_HH__
#define __MEM_CACHE_PREFETCH_VECTOR_BERTI_HH__

#include <deque>
#include <vector>

#include "base/cache/associative_cache.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "mem/cache/prefetch/berti.hh"
#include "mem/cache/tags/tagged_entry.hh"
#include "mem/request.hh"

namespace gem5
{

struct VectorBertiPrefetcherParams;

namespace prefetch
{

class VectorBerti : public Berti
{
  protected:
    /** What Berti's history table does not keep about the vle at one PC. */
    struct VecSizeEntry : public TaggedEntry
    {
        VecSizeEntry(TagExtractor ext);

        /** Forget the vle instructions of the previous PC. */
        void reset();

        /** vl*eew of the latest vle at this PC. */
        unsigned bytes = 0;
        /** Base addresses that already trained the table of deltas. A
         * vle crossing lines has several fills, possibly mixed with those
         * of newer vle at this PC, and must only train once. */
        std::deque<Addr> trainedBases;
    };

    AssociativeCache<VecSizeEntry> vecSizeTable;

    /** Request of the access being notified, as PrefetchInfo has none. */
    RequestPtr curReq;

    struct VectorBertiStats : public statistics::Group
    {
        VectorBertiStats(statistics::Group *parent);

        statistics::Scalar vleTrainings;
        statistics::Scalar vleTriggers;
        statistics::Scalar vleFillsDeduped;
        statistics::Scalar vlePfLines;
        statistics::Scalar vleMultiLineTriggers;
    } statsVec;

    /** Find the entry of a PC, allocating it if needed. */
    VecSizeEntry &findSizeEntry(Addr pc, bool is_secure);

    /** Train once per base address. */
    void trainOnce(VecSizeEntry &size_entry, HistoryTableEntry &entry,
                   Addr base, Cycles latency, Cycles demand_cycle);

    /** Queue every cache line of [base + delta, base + delta + bytes). */
    void sendLines(Addr base, int64_t delta, unsigned bytes,
                   std::vector<AddrPriority> &addresses);

  public:
    VectorBerti(const VectorBertiPrefetcherParams &p);

    void notify(const CacheAccessProbeArg &acc,
                const PrefetchInfo &pfi) override;

    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses,
                           const CacheAccessor &cache) override;

    void notifyFill(const CacheAccessProbeArg &acc) override;
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_VECTOR_BERTI_HH__
