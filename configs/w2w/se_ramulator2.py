"""
gem5 SE-mode config driving Ramulator2 as the memory, with the W2W repair
mechanism active.

This is the TOY VALIDATION step: run a small workload through
    CPU -> caches -> Ramulator2 (repair translator) -> HBM3 timing
and confirm the SAME per-layer repair stats (repair_none / layer_a..d) appear
as in the standalone toy-trace test. Only after that do we point it at GAP.

Usage:
    build/X86/gem5.opt configs/w2w/se_ramulator2.py \
        --cmd /path/to/binary [--options "..."] \
        --ramulator-config /path/to/ramulator2/gem5_hbm3.yaml \
        --ramulator-dir    /path/to/ramulator2 \
        [--cpu-type atomic|timing|o3] [--maxinsts N]

The cache line is 64 B (the x86 norm). The Ramulator2 controller in gem5
splits each line into cacheLineSize / 32 B = 2 HBM3 transactions, and the
LineRoBaRaCoCh mapper keeps both halves in the SAME channel/bank/row, so the
second half is a row-buffer hit -- exactly what a real memory controller does.
"""

import argparse
import os

import m5
from m5.objects import (
    AddrRange,
    Cache,
    L2XBar,
    Process,
    Ramulator2,
    Root,
    SEWorkload,
    SrcClockDomain,
    System,
    SystemXBar,
    VoltageDomain,
)


# ----------------------------------------------------------------- caches
class L1I(Cache):
    size = "32kB"
    assoc = 8
    tag_latency = 2
    data_latency = 2
    response_latency = 2
    mshrs = 16
    tgts_per_mshr = 20


class L1D(Cache):
    size = "32kB"
    assoc = 8
    tag_latency = 2
    data_latency = 2
    response_latency = 2
    mshrs = 16
    tgts_per_mshr = 20
    write_buffers = 16


class L2(Cache):
    size = "1MB"
    assoc = 16
    tag_latency = 20
    data_latency = 20
    response_latency = 20
    mshrs = 32
    tgts_per_mshr = 12
    write_buffers = 8


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cmd", required=True, help="binary to run in SE mode")
    ap.add_argument("--options", default="", help="args for the binary")
    ap.add_argument("--ramulator-config", required=True,
                    help="Ramulator2 YAML (must use Frontend: impl: GEM5)")
    ap.add_argument("--ramulator-dir", default="",
                    help="chdir here so Ramulator2 resolves relative paths "
                         "(e.g. repair_table_path)")
    ap.add_argument("--cpu-type", default="timing",
                    choices=["atomic", "timing", "o3"])
    ap.add_argument("--maxinsts", type=int, default=0)
    ap.add_argument("--mem-size", default="4GB")
    # 64 B is the x86 norm. The Ramulator2 controller splits each line into
    # cacheLineSize / 32 B transactions (must divide evenly).
    ap.add_argument("--cache-line-size", type=int, default=64)
    args = ap.parse_args()

    system = System()
    system.clk_domain = SrcClockDomain(
        clock="3GHz", voltage_domain=VoltageDomain())
    system.mem_mode = "atomic" if args.cpu_type == "atomic" else "timing"
    system.cache_line_size = args.cache_line_size
    system.mem_ranges = [AddrRange(args.mem_size)]

    # -- CPU
    if args.cpu_type == "atomic":
        from m5.objects import X86AtomicSimpleCPU as CPUClass
    elif args.cpu_type == "timing":
        from m5.objects import X86TimingSimpleCPU as CPUClass
    else:
        from m5.objects import X86O3CPU as CPUClass

    system.cpu = CPUClass()
    if args.maxinsts:
        system.cpu.max_insts_any_thread = args.maxinsts

    # -- cache hierarchy
    system.cpu.icache = L1I()
    system.cpu.dcache = L1D()
    system.cpu.icache.cpu_side = system.cpu.icache_port
    system.cpu.dcache.cpu_side = system.cpu.dcache_port

    system.l2bus = L2XBar()
    system.cpu.icache.mem_side = system.l2bus.cpu_side_ports
    system.cpu.dcache.mem_side = system.l2bus.cpu_side_ports

    system.l2cache = L2()
    system.l2cache.cpu_side = system.l2bus.mem_side_ports

    system.membus = SystemXBar()
    system.l2cache.mem_side = system.membus.cpu_side_ports

    system.cpu.createInterruptController()
    system.cpu.interrupts[0].pio = system.membus.mem_side_ports
    system.cpu.interrupts[0].int_requestor = system.membus.cpu_side_ports
    system.cpu.interrupts[0].int_responder = system.membus.mem_side_ports
    system.system_port = system.membus.cpu_side_ports

    # -- THE MEMORY: Ramulator2, with the repair translator inside it
    ram_dir = args.ramulator_dir or os.path.dirname(
        os.path.abspath(args.ramulator_config))
    system.mem_ctrl = Ramulator2(
        configFile=os.path.abspath(args.ramulator_config),
        filePath=os.path.abspath(ram_dir),
        range=system.mem_ranges[0],
    )
    system.mem_ctrl.port = system.membus.mem_side_ports

    # -- workload
    system.workload = SEWorkload.init_compatible(args.cmd)
    process = Process()
    process.cmd = [args.cmd] + (args.options.split() if args.options else [])
    system.cpu.workload = process
    system.cpu.createThreads()

    root = Root(full_system=False, system=system)
    m5.instantiate()

    print(f"[w2w] Ramulator2 config : {args.ramulator_config}")
    print(f"[w2w] cache line size   : {args.cache_line_size} B")
    print(f"[w2w] beginning simulation ({args.cpu_type} CPU)")
    exit_event = m5.simulate()
    print(f"[w2w] exiting @ tick {m5.curTick()} because {exit_event.getCause()}")


main()
