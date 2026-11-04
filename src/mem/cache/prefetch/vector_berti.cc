#include "mem/cache/prefetch/vector_berti.hh"

#include <algorithm>

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/HWPrefetch.hh"
#include "mem/cache/replacement_policies/base.hh"
#include "params/VectorBertiPrefetcher.hh"

namespace gem5
{

namespace prefetch
{

VectorBerti::VecSizeEntry::VecSizeEntry(TagExtractor ext)
  : TaggedEntry()
{
    registerTagExtractor(ext);
    invalidate();
}

void
VectorBerti::VecSizeEntry::reset()
{
    bytes = 0;
    trainedBases.clear();
}

VectorBerti::VectorBertiStats::VectorBertiStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(vleTrainings, statistics::units::Count::get(),
               "Trainings of the table of deltas by a vle"),
      ADD_STAT(vleTriggers, statistics::units::Count::get(),
               "vle instructions that looked for prefetches"),
      ADD_STAT(vleFillsDeduped, statistics::units::Count::get(),
               "vle fills not trained: the instruction already trained"),
      ADD_STAT(vlePfLines, statistics::units::Count::get(),
               "Cache lines queued for vle prefetches"),
      ADD_STAT(vleMultiLineTriggers, statistics::units::Count::get(),
               "vle prefetches that cover more than one cache line")
{
}

VectorBerti::VectorBerti(const VectorBertiPrefetcherParams &p)
    : Berti(p),
      vecSizeTable((name() + ".vecSizeTable").c_str(),
                   p.vec_size_table_entries, p.vec_size_table_assoc,
                   p.vec_size_table_replacement_policy,
                   p.vec_size_table_indexing_policy,
                   VecSizeEntry(
                       genTagExtractor(p.vec_size_table_indexing_policy))),
      statsVec(this)
{
    fatal_if(!p.use_byte_addr,
             "VectorBerti needs deltas between byte addresses.");
}

VectorBerti::VecSizeEntry &
VectorBerti::findSizeEntry(Addr pc, bool is_secure)
{
    const VecSizeEntry::KeyType key{pcHash(pc), is_secure};
    VecSizeEntry *entry = vecSizeTable.findEntry(key);
    if (entry) {
        vecSizeTable.accessEntry(entry);
    } else {
        entry = vecSizeTable.findVictim(key);
        entry->reset();
        vecSizeTable.insertEntry(key, entry);
    }
    return *entry;
}

void
VectorBerti::trainOnce(VecSizeEntry &size_entry, HistoryTableEntry &entry,
                       Addr base, Cycles latency, Cycles demand_cycle)
{
    auto &trained = size_entry.trainedBases;
    if (std::find(trained.begin(), trained.end(), base) != trained.end()) {
        statsVec.vleFillsDeduped++;
        return;
    }
    if (trained.size() >= maxAddrListSize) {
        trained.pop_front();
    }
    trained.push_back(base);

    searchTimelyDeltas(entry, latency, demand_cycle, base);
    statsVec.vleTrainings++;
}

void
VectorBerti::sendLines(Addr base, int64_t delta, unsigned bytes,
                       std::vector<AddrPriority> &addresses)
{
    const Addr pf_base = base + delta;
    const Addr first_line = blockAddress(pf_base);
    const Addr last_line = blockAddress(pf_base + bytes - 1);
    DPRINTF(HWPrefetch, "VectorBerti: base %#x bytes %d delta %d -> lines "
            "%#x..%#x\n", base, bytes, delta, first_line, last_line);
    for (Addr line = first_line; line <= last_line; line += blkSize) {
        addresses.push_back(AddrPriority(line, 0));
        statsVec.vlePfLines++;
    }
    if (first_line != last_line) {
        statsVec.vleMultiLineTriggers++;
    }
}

void
VectorBerti::notify(const CacheAccessProbeArg &acc, const PrefetchInfo &pfi)
{
    curReq = acc.pkt->req;
    Queued::notify(acc, pfi);
    curReq = nullptr;
}

void
VectorBerti::calculatePrefetch(const PrefetchInfo &pfi,
                               std::vector<AddrPriority> &addresses,
                               const CacheAccessor &cache)
{
    if (!pfi.hasPC() || !curReq || !curReq->isVecLoad()) {
        Berti::calculatePrefetch(pfi, addresses, cache);
        return;
    }

    const Addr base = curReq->getVecBaseVaddr();
    const unsigned bytes = curReq->getVecVl() * curReq->getVecEew();
    VecSizeEntry &size_entry = findSizeEntry(pfi.getPC(), pfi.isSecure());
    size_entry.bytes = bytes;

    // A hit on a prefetched block: was the prefetch early enough for this
    // demand? Misses train later, in notifyFill, once the latency is known.
    if (!pfi.isCacheMiss()) {
        HistoryTableEntry *entry = historyTable.findEntry(
            {pcHash(pfi.getPC()), pfi.isSecure()});
        if (entry) {
            auto it = pfLatency.find(blockAddress(pfi.getPaddr()));
            if (it != pfLatency.end()) {
                trainOnce(size_entry, *entry, base, it->second, curCycle());
                pfLatency.erase(it);
                statsBerti.trainOnHit++;
            } else {
                statsBerti.hitSkippedNoLatency++;
            }
        }
    }

    // The base is already in the history for later packets of this vle, so
    // an instruction triggers only once.
    HistoryTableEntry *entry = updateHistoryTable(PrefetchInfo(pfi, base));
    if (!entry) {
        return;
    }
    statsVec.vleTriggers++;

    if (aggressivePF) {
        for (const auto &info : entry->deltas) {
            if (info.status != NO_PREF) {
                sendLines(base, info.delta, size_entry.bytes, addresses);
            }
        }
    } else if (entry->bestDelta.status != NO_PREF) {
        sendLines(base, entry->bestDelta.delta, size_entry.bytes, addresses);
    }
}

void
VectorBerti::notifyFill(const CacheAccessProbeArg &acc)
{
    const RequestPtr &req = acc.pkt->req;

    // Prefetch fills carry no vle information, so Berti records them.
    if (!req->isVecLoad() || !req->hasPC()) {
        Berti::notifyFill(acc);
        return;
    }

    HistoryTableEntry *entry = historyTable.findEntry(
        {pcHash(req->getPC()), req->isSecure()});
    if (!entry) {
        statsBerti.notifySkippedNoEntry++;
        return;
    }

    trainOnce(findSizeEntry(req->getPC(), req->isSecure()), *entry,
              req->getVecBaseVaddr(),
              ticksToCycles(curTick() - req->time()),
              ticksToCycles(req->time()));
    statsBerti.trainOnMiss++;
}

} // namespace prefetch
} // namespace gem5
