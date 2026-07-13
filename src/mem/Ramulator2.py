# Ramulator2 memory controller SimObject.
#
# Modelled on src/mem/DRAMsim3.py. Ramulator2 lives in its own repo and is
# linked as a shared library; see ext/ramulator2/SConscript.

from m5.objects.AbstractMemory import *
from m5.params import *


class Ramulator2(AbstractMemory):
    type = "Ramulator2"
    cxx_header = "mem/ramulator2.hh"
    cxx_class = "gem5::memory::Ramulator2"

    # A single port, matching DRAMsim3.
    port = ResponsePort(
        "port for receiving requests from the CPU or other requestor"
    )

    configFile = Param.String(
        "hbm3.yaml",
        "Ramulator2 YAML config. Must select the GEM5 frontend "
        "(Frontend: impl: GEM5) and may set "
        "MemorySystem.Controller.repair_table_path to a remap_hbm_<id>.json.",
    )

    filePath = Param.String(
        "",
        "Directory to chdir into before Ramulator2 resolves relative paths "
        "(e.g. the ramulator2 checkout). Empty = leave CWD alone.",
    )
