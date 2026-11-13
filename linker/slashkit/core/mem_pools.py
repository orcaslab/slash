# ##################################################################################################
#  The MIT License (MIT)
#  Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
#
#  Permission is hereby granted, free of charge, to any person obtaining a copy of this software
#  and associated documentation files (the "Software"), to deal in the Software without restriction,
#  including without limitation the rights to use, copy, modify, merge, publish, distribute,
#  sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
#  furnished to do so, subject to the following conditions:
#
#  The above copyright notice and this permission notice shall be included in all copies or
#  substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
# NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
# DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
# ##################################################################################################
"""
Pooled whole-HBM memory targets.

A pool is a set of NoC ingress units (NMU512) of the user region that all see the same 32 GB HBM
window. Kernel masters bound to a pool are spread round-robin over its ingress units.

  MEM       8 units in SLR2 (hbm_vnoc_00..07 -> HBM_VNOC_INI_00..07). Every shell has it.
  MEM_SLR0  8 units in SLR0 (hbm_s0_noc_00..07 -> HBM_S0_INI_00..07). Any-SLR HBM shells only.
  MEM_SLR1  8 units in SLR1 (hbm_s1_noc_00..07 -> HBM_S1_INI_00..07). Any-SLR HBM shells only.
  MEM_SLR2  alias of MEM.

Pick the pool of the SLR the master's logic sits in, so HBM traffic crosses dies on the NoC
rather than on SLL fabric.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional


@dataclass(frozen=True)
class MemPool:
    domain: str          # sp= target name, e.g. "MEM_SLR0"
    slr: int
    num_ports: int
    noc_cell_fmt: str    # NoC cell in the user region, formatted with index=
    ini_port_fmt: str    # INI boundary port of the user region, formatted with index=
    sc_base: str         # SmartConnect reduction tree base name
    term_base: str       # terminator base name for unused ingress units
    shell_feature: bool  # True if only any-SLR HBM shells have it

    def noc_cell(self, index: int) -> str:
        return self.noc_cell_fmt.format(index=index)

    def ini_port(self, index: int) -> str:
        return self.ini_port_fmt.format(index=index)

    def noc_pin(self, index: int) -> str:
        return f"/{self.noc_cell(index)}/S00_AXI"

    @property
    def noc_pin_fmt(self) -> str:
        return "/" + self.noc_cell_fmt + "/S00_AXI"


MEM_POOLS: Dict[str, MemPool] = {
    "MEM": MemPool("MEM", 2, 8, "hbm_vnoc_{index:02d}", "HBM_VNOC_INI_{index:02d}",
                   "sc_mem", "axi_register_slice_memterm", False),
    "MEM_SLR0": MemPool("MEM_SLR0", 0, 8, "hbm_s0_noc_{index:02d}", "HBM_S0_INI_{index:02d}",
                        "sc_mem_s0", "axi_register_slice_mems0term", True),
    "MEM_SLR1": MemPool("MEM_SLR1", 1, 8, "hbm_s1_noc_{index:02d}", "HBM_S1_INI_{index:02d}",
                        "sc_mem_s1", "axi_register_slice_mems1term", True),
}

# NoC bandwidth reserved per per-SLR ingress unit (MB/s, read and write). Must equal the shell's
# value (resources/base/compute/scripts/any_slr_hbm.tcl, asl_bw).
POOL_BW_MBPS = 3000

# Targets that are other names for a pool.
MEM_POOL_ALIASES: Dict[str, str] = {"MEM_SLR2": "MEM"}


def canonical_pool(domain: str) -> Optional[str]:
    """Return the pool name for a target domain (resolving aliases), or None."""
    d = domain.strip().upper()
    d = MEM_POOL_ALIASES.get(d, d)
    return d if d in MEM_POOLS else None


def is_pool(domain: Optional[str]) -> bool:
    return domain is not None and canonical_pool(str(domain)) is not None


def shell_has_pool(base_bd: Optional[Path], pool: str) -> bool:
    """True if the installed shell's user-region BD exposes the pool's INI ports."""
    p = MEM_POOLS[pool]
    if not p.shell_feature:
        return True
    if base_bd is None or not Path(base_bd).is_file():
        return False
    return p.ini_port(0) in Path(base_bd).read_text(errors="ignore")


def shell_pools(base_bd: Optional[Path]) -> List[str]:
    """Pools the installed shell provides, in a stable order."""
    return [name for name in MEM_POOLS if shell_has_pool(base_bd, name)]


def used_pools(domains: Iterable[str]) -> List[str]:
    out: List[str] = []
    for d in domains:
        c = canonical_pool(d)
        if c is not None and c not in out:
            out.append(c)
    return out
