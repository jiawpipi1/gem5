/**
 * @file
 * Ramulator2 memory controller for gem5. See ramulator2.hh.
 */

#include "mem/ramulator2.hh"

#include "base/trace.hh"
#include "debug/Drain.hh"
#include "debug/Ramulator2.hh"
#include "sim/core.hh"
#include "sim/system.hh"

namespace gem5
{
namespace memory
{

Ramulator2::Ramulator2(const Params &p) :
    AbstractMemory(p),
    port(name() + ".port", *this),
    wrapper(p.configFile, p.filePath,
            [this](uint64_t addr, bool is_write) {
                requestComplete(addr, is_write);
            }),
    retryReq(false), retryResp(false), startTick(0),
    nbrOutstandingReads(0),
    sendResponseEvent([this]{ sendResponse(); }, name()),
    tickEvent([this]{ tick(); }, name())
{
    DPRINTF(Ramulator2,
            "Instantiated Ramulator2 with config %s (tCK = %f ns)\n",
            p.configFile, wrapper.clockPeriod());

    // Ramulator2 prints its own stats from finalize() -- including the
    // per-channel repair_{none,layer_a..d} counters, which are the whole point
    // of this integration. gem5 never calls it, so hook it to exit.
    registerExitCallback([this]() { wrapper.printStats(); });
}

void
Ramulator2::init()
{
    AbstractMemory::init();

    if (!port.isConnected()) {
        fatal("Ramulator2 %s is unconnected!\n", name());
    } else {
        port.sendRangeChange();
    }

    // A cache line is split into cacheLineSize / burstSize DRAM transactions
    // (HBM3: 64 B / 32 B = 2), so the line must be a whole number of bursts.
    if (system()->cacheLineSize() % wrapper.burstSize() != 0) {
        fatal("Ramulator2: cache line size %d is not a multiple of the DRAM "
              "transaction size %d.\n",
              system()->cacheLineSize(), wrapper.burstSize());
    }
    if (system()->cacheLineSize() == 0 ||
        (system()->cacheLineSize() & (system()->cacheLineSize() - 1)) != 0) {
        fatal("Ramulator2: cache line size must be a positive power of two.\n");
    }
    if (wrapper.interleaveSize() != 0 &&
        system()->cacheLineSize() != wrapper.interleaveSize()) {
        fatal("Ramulator2: gem5 cache line size %d does not match "
              "LineRoBaRaCoCh line_size %d in the Ramulator config.\n",
              system()->cacheLineSize(), wrapper.interleaveSize());
    }
}

void
Ramulator2::startup()
{
    startTick = curTick();
    schedule(tickEvent, clockEdge());
}

void
Ramulator2::resetStats()
{
    startTick = curTick();
}

void
Ramulator2::tick()
{
    // Only drive Ramulator2 in timing mode; atomic/functional accesses bypass
    // it entirely (same as DRAMsim3).
    if (system()->isTimingMode()) {
        wrapper.tick();

        // Retry the transactions Ramulator2's queue refused earlier.
        while (!pendingSends.empty()) {
            const auto &[addr, is_write] = pendingSends.front();
            if (!wrapper.enqueue(addr, is_write)) {
                break;
            }
            DPRINTF(Ramulator2, "Drained pending %s to 0x%x\n",
                    is_write ? "write" : "read", addr);
            pendingSends.pop_front();
        }

        // Ramulator2 gives us no queue-occupancy query, so we cannot predict
        // when space frees up; offer a retry once the pending buffer is clear.
        if (retryReq && pendingSends.empty()) {
            retryReq = false;
            port.sendRetryReq();
        }
        if (nbrOutstanding() == 0 && wrapper.isEmpty()) {
            signalDrainDone();
        }
    }

    schedule(tickEvent,
             curTick() + wrapper.clockPeriod() * sim_clock::as_int::ns);
}

Tick
Ramulator2::recvAtomic(PacketPtr pkt)
{
    access(pkt);

    // Matches DRAMsim3: an arbitrary fixed latency for atomic mode.
    return pkt->cacheResponding() ? 0 : 50000;
}

void
Ramulator2::recvFunctional(PacketPtr pkt)
{
    pkt->pushLabel(name());
    functionalAccess(pkt);
    for (auto i = responseQueue.begin(); i != responseQueue.end(); ++i) {
        pkt->trySatisfyFunctional(*i);
    }
    pkt->popLabel();
}

bool
Ramulator2::recvTimingReq(PacketPtr pkt)
{
    // If a cache is responding, sink the packet without further action.
    if (pkt->cacheResponding()) {
        pendingDelete.reset(pkt);
        return true;
    }

    // We should not get a new request after committing to retry the current
    // one, but the CPU violates this rule, so just ignore it (as DRAMsim3
    // does). Also refuse while a previous packet's transactions are still
    // waiting for space in Ramulator2's queue.
    if (retryReq) {
        return false;
    }
    if (!pendingSends.empty()) {
        retryReq = true;
        return false;
    }

    panic_if(pkt->isWrite() && pkt->isRead(),
             "Ramulator2: packet is both read and write");

    if (!pkt->isRead() && !pkt->isWrite()) {
        // Keep it simple: respond immediately to anything else.
        accessAndRespond(pkt);
        return true;
    }

    const bool is_write = pkt->isWrite();

    // ---- split the packet into DRAM transactions -------------------------
    // A 64 B line on HBM3 becomes 2 consecutive 32 B transactions. With the
    // LineRoBaRaCoCh mapper both land in the same channel/bank/row (adjacent
    // columns), like a real controller issuing back-to-back column commands.
    const Addr burst = wrapper.burstSize();
    const Addr base = pkt->getAddr() & ~(burst - 1);
    const Addr end = pkt->getAddr() + pkt->getSize();
    const unsigned count = (end - base + burst - 1) / burst;

    std::shared_ptr<PacketState> state;
    if (!is_write) {
        state = std::make_shared<PacketState>();
        state->pkt = pkt;
        state->remaining = count;
        // This packet is committed even if Ramulator rejects the first or a
        // later subtransaction. Register every completion before enqueueing so
        // pendingSends can never produce an unmatchable callback.
        for (unsigned i = 0; i < count; ++i) {
            const Addr sub = base + i * burst;
            outstandingReads[sub].push(state);
            ++nbrOutstandingReads;
        }
    }

    for (unsigned i = 0; i < count; i++) {
        const Addr sub = base + i * burst;

        if (!wrapper.enqueue(sub, is_write)) {
            // Queue full mid-split: we have committed to this packet, so
            // buffer the rest and drain from tick(). No new packets are
            // accepted until this empties.
            DPRINTF(Ramulator2, "Ramulator2 full mid-split at 0x%x; "
                    "buffering %d transaction(s)\n", sub, count - i);
            for (unsigned j = i; j < count; j++) {
                pendingSends.emplace_back(base + j * burst, is_write);
            }
            break;
        }
    }

    if (is_write) {
        // Posted write: respond immediately. Ramulator2 models the write's
        // timing internally but never calls back for it (reads only), so
        // there is nothing further to track.
        accessAndRespond(pkt);
    }

    DPRINTF(Ramulator2, "Accepted %s to 0x%x as %d transaction(s) "
            "(%d read transactions outstanding)\n",
            is_write ? "write" : "read", pkt->getAddr(), count,
            nbrOutstandingReads);

    return true;
}

void
Ramulator2::recvRespRetry()
{
    DPRINTF(Ramulator2, "Retrying response\n");
    assert(retryResp);
    retryResp = false;
    sendResponse();
}

void
Ramulator2::accessAndRespond(PacketPtr pkt)
{
    DPRINTF(Ramulator2, "Access for address 0x%x\n", pkt->getAddr());

    bool needsResponse = pkt->needsResponse();

    // Do the actual memory access; this also turns the packet into a response.
    access(pkt);

    if (needsResponse) {
        assert(pkt->isResponse());
        Tick time = curTick() + pkt->headerDelay + pkt->payloadDelay;
        pkt->headerDelay = pkt->payloadDelay = 0;

        responseQueue.push_back(pkt);

        if (!retryResp && !sendResponseEvent.scheduled()) {
            schedule(sendResponseEvent, time);
        }
    } else {
        pendingDelete.reset(pkt);
    }
}

void
Ramulator2::sendResponse()
{
    assert(!retryResp);
    assert(!responseQueue.empty());

    bool success = port.sendTimingResp(responseQueue.front());
    if (success) {
        responseQueue.pop_front();

        if (!responseQueue.empty() && !sendResponseEvent.scheduled()) {
            schedule(sendResponseEvent, curTick());
        }

        if (nbrOutstanding() == 0 && wrapper.isEmpty()) {
            signalDrainDone();
        }
    } else {
        retryResp = true;
        DPRINTF(Ramulator2, "Waiting for response retry\n");
        assert(!sendResponseEvent.scheduled());
    }
}

unsigned int
Ramulator2::nbrOutstanding() const
{
    return nbrOutstandingReads + responseQueue.size() + pendingSends.size();
}

void
Ramulator2::requestComplete(uint64_t addr, bool is_write)
{
    DPRINTF(Ramulator2, "%s to 0x%x complete\n",
            is_write ? "Write" : "Read", addr);

    if (is_write) {
        // Ramulator2's generic controller only invokes callbacks for reads
        // (serve_completed_reads); writes are posted and untracked here. If a
        // future Ramulator2 version starts calling back for writes, there is
        // simply nothing to do.
        return;
    }

    auto p = outstandingReads.find(addr);
    assert(p != outstandingReads.end());

    // FIFO within a transaction address -- not necessarily the true order,
    // but the best we can do since Ramulator2 returns only the address.
    std::shared_ptr<PacketState> state = p->second.front();
    p->second.pop();
    if (p->second.empty()) {
        outstandingReads.erase(p);
    }

    assert(nbrOutstandingReads != 0);
    --nbrOutstandingReads;

    // The packet is done only when ALL of its transactions have completed.
    assert(state->remaining != 0);
    if (--state->remaining == 0) {
        accessAndRespond(state->pkt);
    }

    // If accessAndRespond queued a response, nbrOutstanding() is non-zero and
    // the drain completes from sendResponse() instead. (signalDrainDone is a
    // no-op unless a drain is in progress.)
    if (nbrOutstanding() == 0 && wrapper.isEmpty()) {
        signalDrainDone();
    }
}

Port &
Ramulator2::getPort(const std::string &if_name, PortID idx)
{
    if (if_name != "port") {
        return ClockedObject::getPort(if_name, idx);
    }
    return port;
}

DrainState
Ramulator2::drain()
{
    // Writes are posted and untracked (Ramulator2 has no write callbacks), so
    // draining covers reads, queued responses and not-yet-enqueued splits.
    if (nbrOutstanding() != 0 || !wrapper.isEmpty()) {
        DPRINTF(Drain, "Ramulator2 draining: %d outstanding\n",
                nbrOutstanding());
        return DrainState::Draining;
    }
    return DrainState::Drained;
}

Ramulator2::MemoryPort::MemoryPort(const std::string &_name,
                                   Ramulator2 &_memory)
    : ResponsePort(_name), mem(_memory)
{ }

AddrRangeList
Ramulator2::MemoryPort::getAddrRanges() const
{
    AddrRangeList ranges;
    ranges.push_back(mem.getAddrRange());
    return ranges;
}

Tick
Ramulator2::MemoryPort::recvAtomic(PacketPtr pkt)
{
    return mem.recvAtomic(pkt);
}

void
Ramulator2::MemoryPort::recvFunctional(PacketPtr pkt)
{
    mem.recvFunctional(pkt);
}

bool
Ramulator2::MemoryPort::recvTimingReq(PacketPtr pkt)
{
    return mem.recvTimingReq(pkt);
}

void
Ramulator2::MemoryPort::recvRespRetry()
{
    mem.recvRespRetry();
}

} // namespace memory
} // namespace gem5
