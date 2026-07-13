/**
 * @file
 * Ramulator2 memory controller for gem5.
 *
 * Closely modelled on src/mem/dramsim3.hh, with two structural differences:
 *
 * 1. SPLIT: a gem5 packet (typically a 64 B cache line) is split into
 *    cacheLineSize / burstSize DRAM transactions (HBM3: 2 x 32 B). A read
 *    responds only when ALL its transactions have completed. This is what a
 *    real memory controller does; modelling it (instead of shrinking the cache
 *    line to 32 B) keeps the CPU-side realistic.
 *
 * 2. WRITES ARE POSTED AND UNTRACKED: Ramulator2 invokes completion callbacks
 *    ONLY for reads (see serve_completed_reads() in its generic controller);
 *    writes enter its write buffer and never call back. So we respond to
 *    writes immediately (posted) and do not track their completion --
 *    Ramulator2 still models their full timing effect on the channel
 *    internally. (DRAMsim3 tracked writes only because its library provides
 *    write callbacks.)
 *
 * Ramulator2 also fuses "can you accept?" and "here it is" into a single
 * send() that returns false when its controller queue is full. A split can
 * therefore be accepted partially; the un-sent transactions are buffered in
 * pendingSends and drained each DRAM clock, and new packets are refused
 * (gem5 retry protocol) until the buffer empties.
 */

#ifndef __MEM_RAMULATOR2_HH__
#define __MEM_RAMULATOR2_HH__

#include <deque>
#include <memory>
#include <queue>
#include <unordered_map>
#include <utility>

#include "mem/abstract_mem.hh"
#include "mem/ramulator2_wrapper.hh"
#include "params/Ramulator2.hh"

namespace gem5
{
namespace memory
{

class Ramulator2 : public AbstractMemory
{
  private:
    class MemoryPort : public ResponsePort
    {
      private:
        Ramulator2 &mem;

      public:
        MemoryPort(const std::string &_name, Ramulator2 &_memory);

      protected:
        Tick recvAtomic(PacketPtr pkt);
        void recvFunctional(PacketPtr pkt);
        bool recvTimingReq(PacketPtr pkt);
        void recvRespRetry();
        AddrRangeList getAddrRanges() const;
    };

    MemoryPort port;

    /** The Ramulator2 instance (opaque -- see ramulator2_wrapper.hh). */
    Ramulator2Wrapper wrapper;

    /** Is the connected port waiting for a retry from us? */
    bool retryReq;

    /** Is a response waiting to be retried? */
    bool retryResp;

    Tick startTick;

    /**
     * One entry per read PACKET; shared by all of its DRAM transactions.
     * The packet is responded to when `remaining` reaches zero.
     */
    struct PacketState
    {
        PacketPtr pkt;
        unsigned remaining;
    };

    /**
     * Outstanding read transactions, keyed by TRANSACTION address.
     * Ramulator2 hands back only the address on completion, so we match FIFO
     * within an address (same policy as DRAMsim3).
     */
    std::unordered_map<Addr, std::queue<std::shared_ptr<PacketState>>>
        outstandingReads;

    /** In-flight read transactions (not packets). */
    unsigned int nbrOutstandingReads;

    /**
     * Transactions of an already-accepted packet that Ramulator2's queue
     * refused; retried every DRAM clock. New packets are refused while this
     * is non-empty, so it holds at most one packet's transactions.
     */
    std::deque<std::pair<Addr, bool>> pendingSends;

    /** Responses waiting to be sent back to the requestor. */
    std::deque<PacketPtr> responseQueue;

    unsigned int nbrOutstanding() const;

    /** Perform the access and queue the response. */
    void accessAndRespond(PacketPtr pkt);

    void sendResponse();
    EventFunctionWrapper sendResponseEvent;

    /** Advance Ramulator2 by one memory clock; drain pendingSends. */
    void tick();
    EventFunctionWrapper tickEvent;

    std::unique_ptr<Packet> pendingDelete;

  public:
    typedef Ramulator2Params Params;
    Ramulator2(const Params &p);

    /** Called by the wrapper when Ramulator2 completes a transaction. */
    void requestComplete(uint64_t addr, bool is_write);

    DrainState drain() override;

    virtual Port &getPort(const std::string &if_name,
                          PortID idx = InvalidPortID) override;

    void init() override;
    void startup() override;

    void resetStats() override;

  protected:
    Tick recvAtomic(PacketPtr pkt);
    void recvFunctional(PacketPtr pkt);
    bool recvTimingReq(PacketPtr pkt);
    void recvRespRetry();
};

} // namespace memory
} // namespace gem5

#endif // __MEM_RAMULATOR2_HH__
