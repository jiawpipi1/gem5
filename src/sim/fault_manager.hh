#ifndef __FAULT_MANAGER_HH__
#define __FAULT_MANAGER_HH__

#include <unordered_set>
#include <string>
#include "base/types.hh"

namespace gem5 {

class FaultManager
{
  private:
    std::unordered_set<Addr> faultSet;
    Addr blkSize = 64;

    // Singleton: constructor
    FaultManager() = default;

  public:
    FaultManager(const FaultManager&) = delete;
    FaultManager& operator=(const FaultManager&) = delete;

    static FaultManager& instance()
    {
        static FaultManager _inst;
        return _inst;
    }

    void load(const std::string& path, Addr blk_size = 64);
    bool isFault(Addr a) const;

    void setBlockSize(Addr blk_size) { blkSize = blk_size; }
    Addr getBlockSize() const { return blkSize; }
};

} // namespace gem5

#endif // __FAULT_MANAGER_HH__
