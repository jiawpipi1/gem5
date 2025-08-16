#include "mem/freefault/MeET.hh"

#include <algorithm>
#include "base/logging.hh"
#include "mem/mem_ctrl.hh"
#include "debug/Cache.hh"

namespace gem5 {

MeET::MeET(memory::MemCtrl* o, const Params& p)
    : owner(o), params(p), countPerChip(p.numChips, 0)
{
    fatal_if((params.chipInterleaveBytes == 0) ||
             (params.chipInterleaveBytes &
                (params.chipInterleaveBytes - 1)) != 0,
             "MeET: chipInterleaveBytes must be a power of two (got %u)\n",
             params.chipInterleaveBytes);
}

int
MeET::chipIdOf(Addr lineAddr) const
{
    unsigned shift = 0;
    unsigned b = params.chipInterleaveBytes;
    while ((b & 1u) == 0u) { shift++; b >>= 1; }
    return static_cast<int>((lineAddr >> shift) % params.numChips);
}

void
MeET::incrChipCounter(int chip)
{
    if (chip >= 0 && static_cast<size_t>(chip) < countPerChip.size()) {
        countPerChip[chip]++;
        DPRINTF(Cache, "[MeET] interval no: %lu, incremented"
                        "counter for chip %d, now %u\n",
                        interval_no, chip, countPerChip[chip]);
    } else {
        DPRINTF(Cache, "[MeET] interval no: %lu, invalid chip id"
                        "%d (numChips=%u)\n",
                        interval_no, chip, params.numChips);
    }
}

void
MeET::tryTriggerScrub(int chip)
{
    if (!scrubPending && countPerChip[chip] >= params.retireThreshold) {
        scrubPending = true;
        DPRINTF(Cache, "[MeET] interval no: %lu, threshold reached on chip"
                        "%d (count=%u, threshold=%u) -> trigger scrubber\n",
                        interval_no, chip, countPerChip[chip],
                        params.retireThreshold);
        countPerChip[chip] = 0;
        if (owner) owner->triggerFreeFaultScrubNow();
    }
}

void
MeET::onCorrectableError(Addr physAddr)
{
    const Addr line = lineAlign(physAddr);
    recentError.insert(line);

    const int chip = chipIdOf(line);
    DPRINTF(Cache, "[MeET] interval no: %lu, onCorrectableError"
                    "line=%#lx chip=%d\n",
                    interval_no, line, chip);

    incrChipCounter(chip);
    tryTriggerScrub(chip);
}

void
MeET::startInterval()
{
    interval_no++;
    std::fill(countPerChip.begin(), countPerChip.end(), 0);

    DPRINTF(Cache, "[MeET] start interval no: %lu, reset per-chip"
        "counters\n", interval_no);
}

void
MeET::endInterval()
{
    DPRINTF(Cache, "[MeET] end interval no: %lu\n", interval_no);
    std::fill(countPerChip.begin(), countPerChip.end(), 0);
}

} // namespace gem5
