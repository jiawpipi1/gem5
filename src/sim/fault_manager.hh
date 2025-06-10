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

    /* Singleton：ctor 私有 */
    FaultManager() = default;
  public:
    /* 不可複製 */
    FaultManager(const FaultManager&)            = delete;
    FaultManager& operator=(const FaultManager&) = delete;

    static FaultManager& instance()
    {
        static FaultManager _inst;
        return _inst;
    }
    void load(const std::string& path);
    bool isFault(Addr a) const { return faultSet.count(a); }
};

} // namespace gem5
#endif
