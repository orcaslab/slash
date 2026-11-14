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

MEM units see all 32 HBM pseudo-channels (PCs). A per-SLR unit k serves only PCs
{k, k+8, k+16, k+24} (1 GB each at 0x40_0000_0000 + pc GB); the 8 units of an SLR cover all 32
PCs once. (Full-window per-SLR units add 512 NoC paths, and the NoC compiler cannot solve that.)
Bind a master to one unit with MEM_SLR<s>_<k> and keep its buffers in that unit's PCs (VRT:
an HBM buffer on direct port 2*pc lives in PC pc); MEM_SLR<s> spreads masters round-robin.

Pick the pool of the SLR the master's logic sits in, so HBM traffic crosses dies on the NoC
rather than on SLL fabric.
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple


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
    pcs_per_port: int = 32
    windows: Optional[Dict[int, List[Tuple[int, int]]]] = None  # from the installed shell's BD

    def port_pcs(self, index: int) -> List[int]:
        """HBM pseudo-channels (0..31) the unit serves."""
        if self.windows and index in self.windows:
            pcs: List[int] = []
            for base, size in self.windows[index]:
                first = (base - 0x4000000000) >> 30
                pcs += list(range(first, first + max(1, size >> 30)))
            return pcs
        if self.pcs_per_port >= 32:
            return list(range(32))
        n = 32 // self.pcs_per_port
        return [index % n + m * n for m in range(self.pcs_per_port)]

    def apertures_tcl(self, index: int) -> str:
        """Tcl APERTURES value of the unit's INI port, as the shell sets it."""
        if self.windows and index in self.windows:
            return " ".join("{0x%X %dG}" % (b, z >> 30) for b, z in self.windows[index])
        if self.pcs_per_port >= 32:
            return "{0x40_0000_0000 32G}"
        return " ".join("{0x%X 1G}" % (0x4000000000 + p * 0x40000000) for p in self.port_pcs(index))

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
                        "sc_mem_s0", "axi_register_slice_mems0term", True, 4),
    "MEM_SLR1": MemPool("MEM_SLR1", 1, 8, "hbm_s1_noc_{index:02d}", "HBM_S1_INI_{index:02d}",
                        "sc_mem_s1", "axi_register_slice_mems1term", True, 4),
}

# NoC bandwidth reserved per per-SLR ingress unit (MB/s, read and write). Must equal the shell's
# value (resources/base/compute/scripts/any_slr_hbm.tcl, asl_bw).
POOL_BW_MBPS = 2000

# Targets that are other names for a pool.
MEM_POOL_ALIASES: Dict[str, str] = {"MEM_SLR2": "MEM"}


def canonical_pool(domain: str) -> Optional[str]:
    """Return the pool name for a target domain (resolving aliases), or None."""
    d = domain.strip().upper()
    d = MEM_POOL_ALIASES.get(d, d)
    return d if d in MEM_POOLS else None


def is_pool(domain: Optional[str]) -> bool:
    return domain is not None and canonical_pool(str(domain)) is not None


_SIZE = {"K": 1 << 10, "M": 1 << 20, "G": 1 << 30, "T": 1 << 40}


def _size(v: str) -> int:
    m = re.fullmatch(r"(\d+)([KMGT]?)", v.strip())
    return int(m.group(1)) * _SIZE.get(m.group(2), 1) if m else int(v, 0)


def _memory_maps(obj) -> Dict[str, dict]:
    """All memory_maps entries of a BD JSON (the user-region ports' address blocks)."""
    out: Dict[str, dict] = {}
    if isinstance(obj, dict):
        for k, v in obj.items():
            if k == "memory_maps" and isinstance(v, dict):
                out.update(v)
            else:
                out.update(_memory_maps(v))
    elif isinstance(obj, list):
        for v in obj:
            out.update(_memory_maps(v))
    return out


def shell_port_windows(base_bd: Optional[Path]) -> Dict[str, List[Tuple[int, int]]]:
    """(base, size) windows of every HBM_S<slr>_INI_<k> port in the installed user-region BD."""
    if base_bd is None or not Path(base_bd).is_file():
        return {}
    try:
        maps = _memory_maps(json.loads(Path(base_bd).read_text()))
    except (ValueError, OSError):
        return {}
    out: Dict[str, List[Tuple[int, int]]] = {}
    for port, mm in maps.items():
        if not re.fullmatch(r"HBM_S\d_INI_\d{2}", port):
            continue
        blocks = (mm or {}).get("address_blocks", {})
        out[port] = sorted((int(b["base_address"], 0), _size(b["range"])) for b in blocks.values())
    return out


def shell_pool(base_bd: Optional[Path], name: str) -> Optional[MemPool]:
    """The pool as the installed shell built it (port count and windows read from its BD), or
    None if the shell lacks it. MEM is fixed by every shell."""
    p = MEM_POOLS[name]
    if not p.shell_feature:
        return p
    wins = shell_port_windows(base_bd)
    n = 0
    while p.ini_port(n) in wins:
        n += 1
    if n == 0:
        return None
    pcs = {len(wins[p.ini_port(i)]) for i in range(n)}
    per = 32 if wins[p.ini_port(0)] and wins[p.ini_port(0)][0][1] >= (32 << 30) else max(pcs)
    return replace(p, num_ports=n, pcs_per_port=per, windows={i: wins[p.ini_port(i)] for i in range(n)})


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
