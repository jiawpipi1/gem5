"""
gem5 SE-mode config driving Ramulator2 as the memory, with the W2W repair
mechanism active.

This driver supports both functional validation and ROI-delimited GAP runs through
    CPU -> caches -> Ramulator2 (repair translator) -> HBM3 timing.
The repair table is always explicit: use ``--repair-table none`` for the clean
baseline or provide an audited production JSON. The template's toy table cannot be
selected accidentally.

Usage:
    build/X86/gem5.opt configs/w2w/se_ramulator2.py \
        --cmd /path/to/binary [--options "..."] \
        --ramulator-config /path/to/ramulator2/gem5_hbm3.yaml \
        --ramulator-dir    /path/to/ramulator2 \
        --repair-table none|/path/to/production.json \
        [--repair-lookup-latency DRAM_CYCLES] \
        [--repair-fast-lookup-latency N --repair-slow-lookup-latency N] \
        [--cpu-type atomic|timing|o3] [--maxinsts N]

The cache line is 64 B (the x86 norm). The Ramulator2 controller in gem5
splits each line into cacheLineSize / 32 B = 2 HBM3 transactions, and the
LineRoBaRaCoCh mapper keeps both halves in the SAME channel/bank/row, so the
second half is a row-buffer hit -- exactly what a real memory controller does.
"""

import argparse
import json
import os
import re
import shlex

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


def resolved_ramulator_config(
        config_path, repair_table, repair_lookup_latency=None,
        repair_fast_lookup_latency=None, repair_slow_lookup_latency=None,
        repair_unified_gate=None):
    """Write a per-run config with explicit repair table and latency choices."""
    config_path = os.path.abspath(config_path)
    with open(config_path, "r", encoding="utf-8") as src:
        lines = src.readlines()

    table_lines = [
        i for i, line in enumerate(lines)
        if re.match(r"^\s*repair_table_path\s*:", line)
    ]
    if len(table_lines) != 1:
        raise RuntimeError(
            f"expected exactly one repair_table_path in {config_path}, "
            f"found {len(table_lines)}"
        )

    idx = table_lines[0]
    indent = re.match(r"^(\s*)", lines[idx]).group(1)
    if repair_table == "none":
        lines[idx] = indent + "# repair_table_path: disabled for clean run\n"
        table_desc = "none (clean baseline)"
    else:
        table_path = os.path.abspath(repair_table)
        if not os.path.isfile(table_path):
            raise FileNotFoundError(f"repair table not found: {table_path}")
        lines[idx] = indent + "repair_table_path: " + json.dumps(table_path) + "\n"
        table_desc = table_path

    def replace_int(key, value):
        matches = [
            i for i, line in enumerate(lines)
            if re.match(rf"^\s*{re.escape(key)}\s*:", line)
        ]
        if len(matches) != 1:
            raise RuntimeError(
                f"expected exactly one {key} in {config_path}, "
                f"found {len(matches)}"
            )
        idx = matches[0]
        indent = re.match(r"^(\s*)", lines[idx]).group(1)
        lines[idx] = indent + f"{key}: {value}\n"

    if repair_lookup_latency is not None:
        # Backward-compatible fixed-latency sweep: set both paths when the
        # template uses the split model, otherwise replace the legacy field.
        fast_count = sum(
            bool(re.match(r"^\s*repair_fast_lookup_latency\s*:", line))
            for line in lines
        )
        slow_count = sum(
            bool(re.match(r"^\s*repair_slow_lookup_latency\s*:", line))
            for line in lines
        )
        if fast_count == 1 and slow_count == 1:
            replace_int("repair_fast_lookup_latency", repair_lookup_latency)
            replace_int("repair_slow_lookup_latency", repair_lookup_latency)
        elif fast_count == 0 and slow_count == 0:
            replace_int("repair_lookup_latency", repair_lookup_latency)
        else:
            raise RuntimeError(
                f"incomplete split lookup-latency fields in {config_path}"
            )
    elif repair_fast_lookup_latency is not None:
        replace_int("repair_fast_lookup_latency", repair_fast_lookup_latency)
        replace_int("repair_slow_lookup_latency", repair_slow_lookup_latency)

    if repair_unified_gate is not None:
        # Whether a Bloom reject is fast even when Layer D relocated the request.
        replace_int("repair_unified_gate",
                    "true" if repair_unified_gate else "false")

    os.makedirs(m5.options.outdir, exist_ok=True)
    resolved = os.path.abspath(
        os.path.join(m5.options.outdir, "ramulator2_resolved.yaml")
    )
    with open(resolved, "w", encoding="utf-8") as dst:
        dst.writelines(lines)
    return resolved, table_desc


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
    ap.add_argument(
        "--repair-table", required=True,
        help="'none' for the clean baseline, or an audited production JSON; "
             "must be explicit so the template's toy table is never used",
    )
    ap.add_argument(
        "--repair-lookup-latency", type=int,
        help="set both fast and slow lookup paths to one fixed DRAM-cycle value",
    )
    ap.add_argument(
        "--repair-fast-lookup-latency", type=int,
        help="override the live-bank Bloom-reject latency in DRAM cycles",
    )
    ap.add_argument(
        "--repair-slow-lookup-latency", type=int,
        help="override the dead-bank/Bloom-maybe latency in DRAM cycles",
    )
    ap.add_argument(
        "--repair-gate", choices=["unified", "legacy"], default=None,
        help="unified: a Bloom reject is fast even if Layer D relocated it "
             "(D reads no repair table). legacy: every dead-bank access pays "
             "the slow latency (the conservative bracket).",
    )
    ap.add_argument(
        "--l2-size", default="1MB",
        help="LLC size. Controls how DRAM-bound the kernel is, which is what "
             "actually sets the repair overhead. Shrink it to make a reduced "
             "graph as memory-intensive as a full-size one.",
    )
    ap.add_argument("--cpu-type", default="timing",
                    choices=["atomic", "timing", "o3"])
    ap.add_argument("--maxinsts", type=int, default=0)
    ap.add_argument("--mem-size", default="4GB")
    # 64 B is the x86 norm. The Ramulator2 controller splits each line into
    # cacheLineSize / 32 B transactions (must divide evenly).
    ap.add_argument("--cache-line-size", type=int, default=64)
    args = ap.parse_args()
    if (args.repair_lookup_latency is not None
            and args.repair_lookup_latency < 0):
        ap.error("--repair-lookup-latency must be non-negative")
    split_latency = (args.repair_fast_lookup_latency is not None
                     or args.repair_slow_lookup_latency is not None)
    if args.repair_lookup_latency is not None and split_latency:
        ap.error("fixed and split repair lookup latency options are mutually exclusive")
    if ((args.repair_fast_lookup_latency is None)
            != (args.repair_slow_lookup_latency is None)):
        ap.error("both --repair-fast-lookup-latency and "
                 "--repair-slow-lookup-latency are required together")
    if split_latency and (
            args.repair_fast_lookup_latency < 0
            or args.repair_slow_lookup_latency < args.repair_fast_lookup_latency):
        ap.error("split repair latency must satisfy 0 <= fast <= slow")

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

    # The LLC size sets how DRAM-bound the kernel is, and the repair lookup is
    # charged per DRAM request -- so overhead tracks L2 misses per instruction,
    # not graph scale. Shrinking the LLC lets a reduced graph reproduce the
    # memory intensity of a full-size one. See claude.md S8 "the scale problem".
    system.l2cache = L2()
    system.l2cache.size = args.l2_size
    system.l2cache.cpu_side = system.l2bus.mem_side_ports

    system.membus = SystemXBar()
    system.l2cache.mem_side = system.membus.cpu_side_ports

    system.cpu.createInterruptController()
    system.cpu.interrupts[0].pio = system.membus.mem_side_ports
    system.cpu.interrupts[0].int_requestor = system.membus.cpu_side_ports
    system.cpu.interrupts[0].int_responder = system.membus.mem_side_ports
    system.system_port = system.membus.cpu_side_ports

    # GAP's gem5 build brackets each kernel trial with workbegin/workend.
    # Returning to Python at those markers lets graph loading stay outside ROI.
    system.exit_on_work_items = True

    # -- THE MEMORY: Ramulator2, with the repair translator inside it
    resolved_config, table_desc = resolved_ramulator_config(
        args.ramulator_config, args.repair_table,
        args.repair_lookup_latency,
        args.repair_fast_lookup_latency,
        args.repair_slow_lookup_latency,
        None if args.repair_gate is None else (args.repair_gate == "unified"),
    )
    ram_dir = args.ramulator_dir or os.path.dirname(
        os.path.abspath(args.ramulator_config))
    system.mem_ctrl = Ramulator2(
        configFile=resolved_config,
        filePath=os.path.abspath(ram_dir),
        range=system.mem_ranges[0],
    )
    system.mem_ctrl.port = system.membus.mem_side_ports

    # -- workload
    system.workload = SEWorkload.init_compatible(args.cmd)
    process = Process()
    process.cmd = [args.cmd] + (shlex.split(args.options) if args.options else [])
    system.cpu.workload = process
    system.cpu.createThreads()

    root = Root(full_system=False, system=system)
    m5.instantiate()

    print(f"[w2w] Ramulator2 config : {resolved_config}")
    print(f"[w2w] repair table      : {table_desc}")
    if args.repair_lookup_latency is not None:
        print(f"[w2w] repair lookup     : fixed {args.repair_lookup_latency} DRAM cycles")
    elif split_latency:
        print("[w2w] repair lookup     : "
              f"fast {args.repair_fast_lookup_latency}, "
              f"slow {args.repair_slow_lookup_latency} DRAM cycles")
    else:
        print("[w2w] repair lookup     : template fast/slow values")
    print(f"[w2w] cache line size   : {args.cache_line_size} B")
    print(f"[w2w] beginning simulation ({args.cpu_type} CPU)")
    roi_start = None
    roi_count = 0
    while True:
        exit_event = m5.simulate()
        cause = exit_event.getCause()
        if cause == "workbegin":
            if roi_start is not None:
                raise RuntimeError("nested GAP workbegin annotations")
            roi_start = m5.curTick()
            m5.stats.reset()
            print(f"[w2w] ROI {roi_count} begin @ tick {roi_start}")
            continue
        if cause == "workend":
            if roi_start is None:
                raise RuntimeError("GAP workend without workbegin")
            roi_ticks = m5.curTick() - roi_start
            m5.stats.dump()
            # Ramulator2's counters are NOT part of gem5's statistics system,
            # so m5.stats.dump() does not emit them. Dump them here, while they
            # still hold the ROI, because the m5.stats.reset() on the next line
            # zeroes them (Ramulator2::resetStats -> wrapper.resetStats). Miss
            # this ordering and the only Ramulator block printed is the
            # post-kernel verification tail at process exit.
            system.mem_ctrl.dumpRamulatorStats()
            m5.stats.reset()
            print(f"[w2w] ROI {roi_count} end   @ tick {m5.curTick()} "
                  f"({roi_ticks} ticks)")
            roi_count += 1
            roi_start = None
            continue
        break

    if roi_start is not None:
        raise RuntimeError(f"simulation ended inside GAP ROI: {cause}")
    if roi_count == 0:
        raise RuntimeError(
            "no GAP ROI observed; use a benchmark/GAP build/gem5 binary"
        )
    print(f"[w2w] exiting @ tick {m5.curTick()} because {cause}; "
          f"completed {roi_count} ROI(s)")


main()
