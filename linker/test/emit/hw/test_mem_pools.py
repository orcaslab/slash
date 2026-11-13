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

"""Per-SLR whole-HBM pools: sp=...:MEM_SLR0 / MEM_SLR1 / MEM_SLR2 (= MEM)."""

import re
from pathlib import Path

import pytest

from slashkit.core.bd_ports import load_bd_ports_from_lines, generate_bd_port_lines
from slashkit.core.command_config import LinkerConfiguration, Platform, ShellType
from slashkit.core.kernel import Kernel
from slashkit.core.mem_pools import MEM_POOLS, canonical_pool, shell_pools
from slashkit.core.port import BusType, Port
from slashkit.emit.hw import tcl_gen
from slashkit.emit.metadata.system_map_ctx import _format_target
from slashkit.parser.config_parser import (
    _parse_target,
    apply_config_to_instances,
    parse_connectivity_file,
)


def _kernel(name="tg"):
    ports = {"ap_clk": Port("ap_clk", BusType.CLOCK),
             "ap_rst_n": Port("ap_rst_n", BusType.RESET),
             "m_axi_gmem0": Port("m_axi_gmem0", BusType.AXI4FULL, width=256)}
    return Kernel(name=name, component_xml_path=Path("component.xml"), ports=ports,
                  vlnv="xilinx.com:hls:tg:1.0")


def _shell_bd(tmp_path, any_slr=True):
    p = tmp_path / "slash_base.bd"
    body = '"HBM_VNOC_INI_00": {}'
    if any_slr:
        body += ', "HBM_S0_INI_00": {}, "HBM_S1_INI_00": {}'
    p.write_text("{" + body + "}")
    return p


def _link(tmp_path, monkeypatch, sps, any_slr=True, n=None):
    n = n if n is not None else len(sps)
    lines = ["[connectivity]", "shell=compute", f"nk=tg:{n}:" + ".".join(f"tg_{i}" for i in range(n))]
    lines += [f"sp=tg_{i}.m_axi_gmem0:{t}" for i, t in enumerate(sps)]
    f = tmp_path / "link.cfg"
    f.write_text("\n".join(lines) + "\n")
    cfg = parse_connectivity_file(f)
    k = _kernel()
    conf = LinkerConfiguration.__new__(LinkerConfiguration)
    conf._configuration = cfg
    conf._kernels = [k]
    conf._kernel_instances = apply_config_to_instances(cfg, [k])
    conf._bd_ports = load_bd_ports_from_lines(generate_bd_port_lines(num_ddr=12, num_virt=0))
    conf._build_dir = tmp_path
    conf._project_name = "pools"
    conf._clock_hz = 200_000_000
    conf._platform = Platform("hw")
    assert conf.shell_type == ShellType.COMPUTE
    bd = _shell_bd(tmp_path, any_slr)
    monkeypatch.setattr(tcl_gen, "installed_base_bd", lambda shell_type: bd)
    tcl_gen.generate_tcl(conf)
    return re.sub(r"\\\n\s*", " ", (tmp_path / "slash.tcl").read_text())


def test_parse_pool_targets():
    assert _parse_target("MEM_SLR0").domain == "MEM_SLR0"
    assert _parse_target("mem_slr1").domain == "MEM_SLR1"
    assert _parse_target("MEM_SLR2").domain == "MEM"   # alias: the SLR2 vNoC ports
    assert _parse_target("MEM").domain == "MEM"
    with pytest.raises(ValueError):
        _parse_target("MEM_SLR3")


def test_pool_names():
    assert canonical_pool("MEM_SLR2") == "MEM"
    assert MEM_POOLS["MEM"].noc_pin(3) == "/hbm_vnoc_03/S00_AXI"     # unchanged MEM pins
    assert MEM_POOLS["MEM_SLR0"].ini_port(7) == "HBM_S0_INI_07"
    assert MEM_POOLS["MEM_SLR1"].noc_pin(0) == "/hbm_s1_noc_00/S00_AXI"


def test_shell_detection(tmp_path):
    assert shell_pools(_shell_bd(tmp_path, True)) == ["MEM", "MEM_SLR0", "MEM_SLR1"]
    assert shell_pools(_shell_bd(tmp_path, False)) == ["MEM"]
    assert shell_pools(None) == ["MEM"]


def test_format_target():
    assert _format_target("MEM", 3) == "MEM3"
    assert _format_target("MEM_SLR0", 3) == "MEM_SLR0_3"
    assert _format_target("HBM", 12) == "HBM12"


def test_pools_round_robin_and_cells(tmp_path, monkeypatch):
    sps = ["MEM_SLR0"] * 9 + ["MEM_SLR1"] * 2 + ["MEM_SLR2"]
    t = _link(tmp_path, monkeypatch, sps)
    # SLR0: 9 masters on 8 units -> unit 0 gets a 2-input SmartConnect, units 1..7 direct.
    assert re.search(r"smartconnect:1.0 sc_mem_s0_0_0_0", t)
    for i in range(1, 8):
        assert re.search(rf"connect_bd_intf_net \[get_bd_intf_pins tg_{i}/m_axi_gmem0\] "
                         rf"\[get_bd_intf_pins /hbm_s0_noc_{i:02d}/S00_AXI\]", t)
    # SLR1: two masters direct on units 0 and 1, the rest terminated.
    assert re.search(r"\[get_bd_intf_pins tg_9/m_axi_gmem0\] \[get_bd_intf_pins /hbm_s1_noc_00/S00_AXI\]", t)
    assert re.search(r"\[get_bd_intf_pins tg_10/m_axi_gmem0\] \[get_bd_intf_pins /hbm_s1_noc_01/S00_AXI\]", t)
    for i in range(2, 8):
        assert re.search(rf"axi_register_slice_mems1term_\d+/M_AXI\] \[get_bd_intf_pins /hbm_s1_noc_{i:02d}/S00_AXI\]", t)
    # MEM_SLR2 is MEM: hbm_vnoc_00, other vNoC units terminated as before.
    assert re.search(r"\[get_bd_intf_pins tg_11/m_axi_gmem0\] \[get_bd_intf_pins /hbm_vnoc_00/S00_AXI\]", t)
    assert re.search(r"axi_register_slice_memterm_\d+/M_AXI\] \[get_bd_intf_pins /hbm_vnoc_07/S00_AXI\]", t)
    # Every per-SLR NoC cell of the shell is recreated, wired to its INI port, clocked, windowed.
    for s in (0, 1):
        for i in range(8):
            c, p = f"hbm_s{s}_noc_{i:02d}", f"HBM_S{s}_INI_{i:02d}"
            assert re.search(rf"create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc {c} ", t)
            assert re.search(rf"connect_bd_intf_net \[get_bd_intf_ports {p}\] \[get_bd_intf_pins {c}/M00_INI\]", t)
            assert re.search(rf"connect_bd_net \[get_bd_pins user_clk\] \[get_bd_pins {c}/aclk0\]", t)
            assert re.search(rf"set_property APERTURES \{{\{{0x40_0000_0000 32G\}}\}} \[get_bd_intf_ports {p}\]", t)
    assert "read_bw {3000} write_bw {3000}" in t


def test_old_shell_unchanged(tmp_path, monkeypatch):
    """On a shell without per-SLR ports nothing new is emitted, and MEM binds as before."""
    t = _link(tmp_path, monkeypatch, ["MEM", "MEM"], any_slr=False)
    assert "hbm_s0_noc" not in t and "hbm_s1_noc" not in t and "HBM_S0_INI" not in t
    assert re.search(r"\[get_bd_intf_pins tg_0/m_axi_gmem0\] \[get_bd_intf_pins /hbm_vnoc_00/S00_AXI\]", t)
    assert re.search(r"\[get_bd_intf_pins tg_1/m_axi_gmem0\] \[get_bd_intf_pins /hbm_vnoc_01/S00_AXI\]", t)


def test_pool_on_old_shell_fails(tmp_path, monkeypatch):
    with pytest.raises(ValueError, match="any-SLR HBM static shell"):
        _link(tmp_path, monkeypatch, ["MEM_SLR0"], any_slr=False)
