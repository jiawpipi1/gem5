#include "sim/fault_manager.hh"
#include <fstream>
#include "base/trace.hh"
#include <nlohmann/json.hpp>
#include "debug/Cache.hh"

namespace gem5 {

void
FaultManager::load(const std::string& path, Addr blk_size)
{
    blkSize = blk_size;

    std::ifstream fin(path);
    if (!fin.is_open())
        panic("Cannot open fault-file %s\n", path);

    nlohmann::json j;
    fin >> j;

    for (auto& e : j) {
        Addr raw = e.get<Addr>();
        Addr aligned = raw & ~(blkSize - 1);
        faultSet.insert(aligned);
        DPRINTF(Cache,
            "[FaultManager] Loaded fault line address (aligned):"
            "%#lx (raw: %#lx)\n",
            aligned, raw);
    }

    inform("FaultManager: loaded %zu aligned faulty lines\n", faultSet.size());
}

bool
FaultManager::isFault(Addr a) const
{
    Addr aligned = a & ~(blkSize - 1);
    bool result = faultSet.count(aligned) > 0;
    DPRINTF(Cache, "[FaultManager] Check addr=%#lx aligned=%#lx => %s\n",
            a, aligned, result ? "FAULT" : "OK");
    return result;
}

void
FaultManager::markFault(Addr a)
{
    Addr aligned = a & ~(blkSize - 1);
    if (faultSet.count(aligned) == 0) {
        faultSet.insert(aligned);
        DPRINTF(Cache,
            "[FaultManager] Dynamically marked fault at %#lx (aligned)\n",
            aligned);
    }
}


} // namespace gem5
