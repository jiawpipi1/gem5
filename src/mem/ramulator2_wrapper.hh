/**
 * @file
 * Ramulator2Wrapper declaration.
 *
 * This class deliberately exposes ONLY plain C++ types. Ramulator2 and gem5
 * both define a class called `Request` (and both have `Stats`), so the two
 * header sets must never be included in the same translation unit. Everything
 * Ramulator2-specific is hidden behind opaque pointers here and included only
 * in ramulator2_wrapper.cc.
 *
 * Modelled on dramsim3_wrapper.hh.
 */

#ifndef __MEM_RAMULATOR2_WRAPPER_HH__
#define __MEM_RAMULATOR2_WRAPPER_HH__

#include <cstdint>
#include <functional>
#include <string>

namespace gem5
{
namespace memory
{

/**
 * Wrapper class to avoid having to include Ramulator2 headers in gem5 code.
 */
class Ramulator2Wrapper
{
  private:
    // Opaque: Ramulator::IFrontEnd* / Ramulator::IMemorySystem*.
    void *frontend;
    void *memory_system;

    double _clockPeriod;     ///< DRAM clock period in ns (from Ramulator's tCK)
    unsigned int _burstSize; ///< bytes per transaction
    unsigned int _interleaveSize; ///< LineRoBaRaCoCh line_size, or 0

    /// Called by Ramulator2 when a request completes.
    std::function<void(uint64_t, bool)> complete_cb;

  public:
    /**
     * @param config_file  Ramulator2 YAML config (must select the GEM5 frontend)
     * @param working_dir  directory to resolve relative paths against
     * @param complete_cb  invoked as (addr, is_write) when a request completes
     */
    Ramulator2Wrapper(const std::string &config_file,
                      const std::string &working_dir,
                      std::function<void(uint64_t, bool)> complete_cb);

    ~Ramulator2Wrapper();

    void setCallback(std::function<void(uint64_t, bool)> cb);

    /**
     * Hand a request to Ramulator2.
     *
     * Ramulator2 fuses "can you accept?" and "here it is" into a single
     * send() that returns false when the controller queue is full, so unlike
     * DRAMsim3 there is no separate canAccept(). Callers must honour the
     * return value and use gem5's retry protocol.
     *
     * @return true if accepted; false if the memory controller is full.
     */
    bool enqueue(uint64_t addr, bool is_write);

    /** Advance Ramulator2 by one memory-system clock. */
    void tick();

    /** True when nothing is in flight inside Ramulator2. */
    bool isEmpty() const;

    /** Print Ramulator2's stats (its finalize()). */
    void printStats();

    double clockPeriod() const { return _clockPeriod; }
    unsigned int burstSize() const { return _burstSize; }
    unsigned int interleaveSize() const { return _interleaveSize; }
};

} // namespace memory
} // namespace gem5

#endif // __MEM_RAMULATOR2_WRAPPER_HH__
