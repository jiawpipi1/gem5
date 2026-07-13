/**
 * @file
 * Ramulator2Wrapper implementation.
 *
 * This is the ONLY translation unit that sees Ramulator2's headers. gem5's
 * headers must not appear here: both projects define `Request`, so including
 * both would be ambiguous at best.
 */

#include "mem/ramulator2_wrapper.hh"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

#include <unistd.h>   // chdir

// --- Ramulator2 headers (NOT gem5 headers) ---------------------------------
#include "base/base.h"
#include "base/config.h"
#include "base/request.h"
#include "frontend/frontend.h"
#include "memory_system/memory_system.h"

namespace gem5
{
namespace memory
{

Ramulator2Wrapper::Ramulator2Wrapper(
        const std::string &config_file,
        const std::string &working_dir,
        std::function<void(uint64_t, bool)> cb)
    : frontend(nullptr), memory_system(nullptr),
      _clockPeriod(0.0), _burstSize(0), complete_cb(cb)
{
    // Ramulator2 resolves some paths (e.g. the repair table) relative to CWD.
    if (!working_dir.empty()) {
        if (chdir(working_dir.c_str()) != 0) {
            throw std::runtime_error(
                "Ramulator2Wrapper: cannot chdir to " + working_dir);
        }
    }

    std::vector<std::string> no_overrides;
    YAML::Node config =
        Ramulator::Config::parse_config_file(config_file, no_overrides);

    auto *fe = Ramulator::Factory::create_frontend(config);
    auto *ms = Ramulator::Factory::create_memory_system(config);

    if (!fe || !ms) {
        throw std::runtime_error(
            "Ramulator2Wrapper: failed to build frontend/memory system from " +
            config_file);
    }

    fe->connect_memory_system(ms);
    ms->connect_frontend(fe);

    frontend = static_cast<void *>(fe);
    memory_system = static_cast<void *>(ms);

    // tCK is in nanoseconds; gem5 wants a clock period.
    float tCK = ms->get_tCK();
    if (tCK <= 0.0f) {
        throw std::runtime_error(
            "Ramulator2Wrapper: memory system reported a non-positive tCK. "
            "Does the config select a real DRAM device?");
    }
    _clockPeriod = static_cast<double>(tCK);

    // Ramulator2 does not expose a transaction size directly. For HBM3 the
    // access granularity is prefetch x channel_width/8 = 2 x 128/8 = 32 B.
    // gem5's cache line is what actually drives request size, so this is only
    // used for a sanity warning in ramulator2.cc.
    _burstSize = 32;
}

Ramulator2Wrapper::~Ramulator2Wrapper()
{
    // Ramulator2's Factory owns these; it has no destroy() API and the process
    // is exiting anyway. Deliberately not deleting to avoid a double free.
}

void
Ramulator2Wrapper::setCallback(std::function<void(uint64_t, bool)> cb)
{
    complete_cb = cb;
}

bool
Ramulator2Wrapper::enqueue(uint64_t addr, bool is_write)
{
    auto *fe = static_cast<Ramulator::IFrontEnd *>(frontend);

    int type_id = is_write ? Ramulator::Request::Type::Write
                           : Ramulator::Request::Type::Read;

    // Ramulator2 hands the completed Request back to this lambda. We only need
    // the address and direction to match it to the outstanding gem5 packet.
    auto cb = [this, is_write](Ramulator::Request &req) {
        if (complete_cb) {
            complete_cb(static_cast<uint64_t>(req.addr), is_write);
        }
    };

    // Returns false when the controller queue is full -> caller must retry.
    return fe->receive_external_requests(type_id,
                                         static_cast<Ramulator::Addr_t>(addr),
                                         0 /* source_id */, cb);
}

void
Ramulator2Wrapper::tick()
{
    static_cast<Ramulator::IMemorySystem *>(memory_system)->tick();
}

bool
Ramulator2Wrapper::isEmpty() const
{
    return static_cast<Ramulator::IMemorySystem *>(memory_system)->is_empty();
}

void
Ramulator2Wrapper::printStats()
{
    // finalize() is what prints Ramulator2's YAML stats block, including the
    // per-channel repair_{none,layer_a..d} counters we care about.
    static_cast<Ramulator::IFrontEnd *>(frontend)->finalize();
    static_cast<Ramulator::IMemorySystem *>(memory_system)->finalize();
}

} // namespace memory
} // namespace gem5
