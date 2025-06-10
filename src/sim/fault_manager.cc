#include "sim/fault_manager.hh"
#include <fstream>
#include "base/trace.hh"
#include <nlohmann/json.hpp>
#include "debug/Cache.hh"

namespace gem5 {

void
FaultManager::load(const std::string& path)
{
    std::ifstream fin(path);
    if (!fin.is_open())
        panic("Cannot open fault-file %s\n", path);

    nlohmann::json j;
    fin >> j;

    for (auto& e : j) {
        Addr addr = e.get<Addr>();
        faultSet.insert(e.get<Addr>());
        DPRINTF(Cache,
            "[FaultManager] Loaded fault line address: %#lx\n",
            addr);
    }

    inform("FaultManager: loaded %zu faulty lines\n", faultSet.size());
}

} // namespace gem5
