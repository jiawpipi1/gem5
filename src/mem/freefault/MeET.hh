#ifndef __FREEFAULT_MEET_HH__
#define __FREEFAULT_MEET_HH__

#include <vector>
#include <unordered_set>
#include "base/types.hh"

namespace gem5 {
namespace memory { class MemCtrl; }
class MeET
{
  public:
    struct Params {
        unsigned cacheLineBytes      = 64;
        unsigned numChips            = 8;
        unsigned chipInterleaveBytes = 8;
        unsigned retireThreshold     = 16;
        Tick     intervalTicks       = 5e5;
    };

    MeET(memory::MemCtrl* owner, const Params& p);


    void onCorrectableError(Addr physAddr);


    void startInterval();
    void endInterval();


    bool shouldTriggerScrub() const { return scrubPending; }
    void clearScrubPending()        { scrubPending = false; }


    const std::unordered_set<Addr>& recentErrorLines() const { return recentError; }
    void clearRecentErrorLines() { recentError.clear(); }


    inline Addr lineAlign(Addr a) const { return a & ~(Addr(params.cacheLineBytes - 1)); }
    int    chipIdOf(Addr lineAddr) const;
    const Params& getParams() const { return params; }
    uint64_t intervalNo() const { return interval_no; }

  private:
    memory::MemCtrl* owner;
    Params   params;

    std::vector<uint32_t> countPerChip;
    bool scrubPending = false;

    std::unordered_set<Addr> recentError;
    uint64_t interval_no = 0;

    void incrChipCounter(int chip);
    void tryTriggerScrub(int chip);
};

} // namespace gem5

#endif // __FREEFAULT_MEET_HH__
