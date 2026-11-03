/*
 * Adapted from the Berti prefetcher in OpenXiangShan/GEM5
 * (src/mem/cache/prefetch/berti.{cc,hh}, BSD-3-Clause). See berti.hh.
 */

#include "mem/cache/prefetch/berti.hh"

#include <algorithm>
#include <cstdlib>
#include <string>

#include "base/trace.hh"
#include "debug/HWPrefetch.hh"
#include "mem/cache/replacement_policies/base.hh"
#include "params/BertiPrefetcher.hh"

namespace gem5
{

namespace prefetch
{

Berti::HistoryTableEntry::HistoryTableEntry(int num_deltas, TagExtractor ext)
  : TaggedEntry(), deltas(num_deltas)
{
    registerTagExtractor(ext);
    invalidate();
}

void
Berti::HistoryTableEntry::resetForNewPC(Addr pc_key, bool is_secure)
{
    std::fill(deltas.begin(), deltas.end(), DeltaInfo());
    bestDelta = DeltaInfo();
    counter = 0;
    hysteresis = false;
    pcKey = pc_key;
    secure = is_secure;
    history.clear();
}

void
Berti::HistoryTableEntry::resetConfidence()
{
    counter = 0;
    for (auto &info : deltas) {
        info.coverageCounter = 0;
    }
}

void
Berti::HistoryTableEntry::updateStatus()
{
    uint8_t max_cov = 0;
    for (auto &info : deltas) {
        info.status = (info.coverageCounter >= 2) ? L2_PREF : NO_PREF;
        info.status = (info.coverageCounter >= 4) ? L1_PREF : info.status;
        if (info.status != NO_PREF && info.coverageCounter > max_cov) {
            max_cov = info.coverageCounter;
            bestDelta = info;
        }
    }
    if (max_cov == 0) {
        bestDelta.delta = 0;
        bestDelta.status = NO_PREF;
    }
}

Berti::BertiStats::BertiStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(trainOnHit, statistics::units::Count::get(),
               "Trainings on a hit on a prefetched block"),
      ADD_STAT(trainOnMiss, statistics::units::Count::get(),
               "Trainings on a demand fill"),
      ADD_STAT(notifySkippedCond1, statistics::units::Count::get(),
               "Fills skipped: instruction fetch, no vaddr or no PC"),
      ADD_STAT(notifySkippedNoEntry, statistics::units::Count::get(),
               "Demand fills skipped: no history entry for the PC"),
      ADD_STAT(entryEvicts, statistics::units::Count::get(),
               "History entries with a usable delta that were evicted"),
      ADD_STAT(pfLatencyRecorded, statistics::units::Count::get(),
               "Prefetch fills whose latency was recorded"),
      ADD_STAT(hitSkippedNoLatency, statistics::units::Count::get(),
               "Hits on prefetched blocks not trained: latency unknown"),
      ADD_STAT(deltasUntimely, statistics::units::Count::get(),
               "History addresses rejected as not timely")
{
}

Berti::Berti(const BertiPrefetcherParams &p)
    : Queued(p),
      maxAddrListSize(p.addrlist_size),
      maxDeltaFound(p.max_deltafound),
      pfLatencyEntries(p.pf_latency_entries),
      aggressivePF(p.aggressive_pf),
      useByteAddr(p.use_byte_addr),
      historyTable((name() + ".historyTable").c_str(),
                   p.history_table_entries, p.history_table_assoc,
                   p.history_table_replacement_policy,
                   p.history_table_indexing_policy,
                   HistoryTableEntry(p.deltalist_size,
                       genTagExtractor(p.history_table_indexing_policy))),
      statsBerti(this)
{
}

Berti::HistoryTableEntry *
Berti::updateHistoryTable(const PrefetchInfo &pfi)
{
    const Addr training_addr = trainingAddr(pfi.getAddr());
    const Addr pc_key = pcHash(pfi.getPC());
    const HistoryTableEntry::KeyType key{pc_key, pfi.isSecure()};
    const HistoryInfo new_info{training_addr, curCycle()};

    HistoryTableEntry *entry = historyTable.findEntry(key);
    if (entry) {
        historyTable.accessEntry(entry);

        const bool found = std::any_of(
            entry->history.begin(), entry->history.end(),
            [&](const HistoryInfo &h) { return h.vAddr == training_addr; });
        if (found) {
            DPRINTF(HWPrefetch, "Berti: PC %#x addr %#x already in history\n",
                    pfi.getPC(), training_addr);
            return nullptr;
        }
        if (entry->history.size() >= maxAddrListSize) {
            entry->history.pop_front();
        }
        entry->history.push_back(new_info);
        entry->hysteresis = true;
        return entry;
    }

    entry = historyTable.findVictim(key);
    if (entry->hysteresis) {
        // Second chance: keep the old PC, and do not allocate the new one.
        entry->hysteresis = false;
        const HistoryTableEntry::KeyType old_key{entry->pcKey, entry->secure};
        historyTable.insertEntry(old_key, entry);
        return nullptr;
    }

    if (entry->bestDelta.status != NO_PREF) {
        statsBerti.entryEvicts++;
    }
    entry->resetForNewPC(pc_key, pfi.isSecure());
    entry->history.push_back(new_info);
    historyTable.insertEntry(key, entry);
    return nullptr;
}

void
Berti::searchTimelyDeltas(HistoryTableEntry &entry, Cycles latency,
                          Cycles demand_cycle, Addr trigger_addr)
{
    DPRINTF(HWPrefetch, "Berti: latency %d demand cycle %d, %d in history\n",
            latency, demand_cycle, entry.history.size());

    std::vector<int64_t> new_deltas;
    const int64_t delta_thres = useByteAddr ? blkSize : 8;
    for (auto it = entry.history.rbegin(); it != entry.history.rend(); it++) {
        const int64_t delta = (int64_t)trigger_addr - (int64_t)it->vAddr;

        // Skip short deltas
        if (std::llabs(delta) <= delta_thres) {
            continue;
        }

        // A prefetch issued at this history address would have been late
        if (it->timestamp + latency >= demand_cycle) {
            statsBerti.deltasUntimely++;
            continue;
        }

        new_deltas.push_back(delta);
        if (new_deltas.size() >= maxDeltaFound) {
            break;
        }
    }

    entry.counter++;

    for (const int64_t delta : new_deltas) {
        auto hit = std::find_if(entry.deltas.begin(), entry.deltas.end(),
            [&](const DeltaInfo &info) {
                return info.coverageCounter != 0 && info.delta == delta;
            });
        if (hit != entry.deltas.end()) {
            hit->coverageCounter++;
            continue;
        }

        // Replace the delta with the smallest coverage
        auto victim = entry.deltas.begin();
        for (auto it = entry.deltas.begin(); it != entry.deltas.end(); it++) {
            if (victim->coverageCounter >= it->coverageCounter) {
                victim = it;
            }
        }
        victim->delta = delta;
        victim->coverageCounter = 1;
        victim->status = NO_PREF;
        DPRINTF(HWPrefetch, "Berti: new delta %d\n", delta);
    }

    if (entry.counter >= 6) {
        entry.updateStatus();
        if (entry.counter >= 16) {
            entry.resetConfidence();
        }
    }
}

void
Berti::sendPrefetch(const PrefetchInfo &pfi, int64_t delta,
                    std::vector<AddrPriority> &addresses) const
{
    const Addr pf_addr = useByteAddr ? pfi.getAddr() + delta :
        (blockIndex(pfi.getAddr()) + delta) << lBlkSize;
    DPRINTF(HWPrefetch, "Berti: PC %#x addr %#x delta %d -> %#x\n",
            pfi.getPC(), pfi.getAddr(), delta, pf_addr);
    addresses.push_back(AddrPriority(pf_addr, 0));
}

void
Berti::calculatePrefetch(const PrefetchInfo &pfi,
                         std::vector<AddrPriority> &addresses,
                         const CacheAccessor &cache)
{
    if (!pfi.hasPC()) {
        DPRINTF(HWPrefetch, "Berti: ignoring request with no PC.\n");
        return;
    }

    // A hit on a prefetched block: was the prefetch early enough for this
    // demand? Misses train later, in notifyFill, once the latency is known.
    if (!pfi.isCacheMiss()) {
        HistoryTableEntry *entry = historyTable.findEntry(
            {pcHash(pfi.getPC()), pfi.isSecure()});
        if (entry) {
            auto it = pfLatency.find(blockAddress(pfi.getPaddr()));
            if (it != pfLatency.end()) {
                searchTimelyDeltas(*entry, it->second, curCycle(),
                                   trainingAddr(pfi.getAddr()));
                pfLatency.erase(it);
                statsBerti.trainOnHit++;
            } else {
                statsBerti.hitSkippedNoLatency++;
            }
        }
    }

    HistoryTableEntry *entry = updateHistoryTable(pfi);
    if (!entry) {
        return;
    }

    if (aggressivePF) {
        for (const auto &info : entry->deltas) {
            if (info.status != NO_PREF) {
                sendPrefetch(pfi, info.delta, addresses);
            }
        }
    } else if (entry->bestDelta.status != NO_PREF) {
        sendPrefetch(pfi, entry->bestDelta.delta, addresses);
    }
}

void
Berti::notifyFill(const CacheAccessProbeArg &acc)
{
    const PacketPtr pkt = acc.pkt;
    const RequestPtr &req = pkt->req;

    // Prefetch requests carry no PREFETCH flag and no vaddr (see
    // Queued::DeferredPacket::createPkt), so identify them by requestor.
    if (req->requestorId() == requestorId) {
        const Addr blk_addr = pkt->getBlockAddr(blkSize);
        if (pfLatency.find(blk_addr) == pfLatency.end()) {
            pfLatencyOrder.push_back(blk_addr);
            if (pfLatencyOrder.size() > pfLatencyEntries) {
                pfLatency.erase(pfLatencyOrder.front());
                pfLatencyOrder.pop_front();
            }
        } //least used
        pfLatency[blk_addr] = ticksToCycles(curTick() - req->time());
        statsBerti.pfLatencyRecorded++;
        return;
    }

    if (req->isInstFetch() || !req->hasVaddr() || !req->hasPC()) {
        statsBerti.notifySkippedCond1++;
        return;
    }

    HistoryTableEntry *entry = historyTable.findEntry(
        {pcHash(req->getPC()), req->isSecure()});
    if (!entry) {
        statsBerti.notifySkippedNoEntry++;
        return;
    }

    searchTimelyDeltas(*entry, ticksToCycles(curTick() - req->time()),
                       ticksToCycles(req->time()),
                       trainingAddr(req->getVaddr()));
    statsBerti.trainOnMiss++;
}

} // namespace prefetch
} // namespace gem5
