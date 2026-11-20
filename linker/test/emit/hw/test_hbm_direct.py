# ##################################################################################################
#  The MIT License (MIT)
#  Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
#  (See the repository's LICENSE.)
# ##################################################################################################

"""sp=<inst>.<port>:HBM<k>:DIRECT and clk=<inst>.<port>:STATIC: a kernel master that presents the HBM
port's width (256 bits) and clock (the static region clock) is wired straight to HBM_AXI_<k>, with no
SmartConnect; its clock input is driven by the static region clock."""

from pathlib import Path
import pytest

from slashkit.parser.config_parser import _parse_sp_value, _parse_clk_value, apply_config_to_instances
from slashkit.core.kernel import Kernel
from slashkit.core.port import BusType, Port
from slashkit.core.connectivity import ConnectivityConfig, NKSpec, SpMapping, MemoryTarget, ClockBind
from slashkit.emit.hw.user_region.hbm_ctx import build_hbm_smartconnect_context
from slashkit.emit.hw.user_region.kernel_ctx import build_kernel_add_context


def _kernel(width=256):
    ports = {"m_axi_hbm": Port(name="m_axi_hbm", ptype=BusType.AXI4FULL, width=width),
             "m_axi_mem": Port(name="m_axi_mem", ptype=BusType.AXI4FULL, width=512),
             "ap_clk": Port(name="ap_clk", ptype=BusType.CLOCK),
             "hbm_clk": Port(name="hbm_clk", ptype=BusType.CLOCK)}
    return Kernel(name="k", component_xml_path=Path(), ports=ports)


class _BD:
    class _P:
        def __init__(self, name):
            self.name = name
            self.rtl_name = name

    def mem(self, dom, idx):
        return self._P(f"HBM_AXI_{idx:02d}")


def test_parse_direct_and_clk():
    sp = _parse_sp_value("k_0.m_axi_hbm:HBM12:DIRECT")
    assert sp.direct and sp.target == MemoryTarget("HBM", 12)
    assert not _parse_sp_value("k_0.m_axi_hbm:HBM12").direct
    with pytest.raises(ValueError):
        _parse_sp_value("k_0.m_axi_mem:MEM:DIRECT")
    assert _parse_clk_value("k_0.hbm_clk:STATIC") == ClockBind("k_0", "hbm_clk", "static_region_clk")
    with pytest.raises(ValueError):
        _parse_clk_value("k_0.hbm_clk:USER")


def _apply(width=256, direct=True, two=False):
    sps = [SpMapping("k_0", "m_axi_hbm", MemoryTarget("HBM", 12), direct=direct)]
    if two:
        sps.append(SpMapping("k_0", "m_axi_mem", MemoryTarget("HBM", 12)))
    cfg = ConnectivityConfig(nk=[NKSpec("k", 1, ["k_0"])], sps=sps,
                             clock_binds=[ClockBind("k_0", "hbm_clk", "static_region_clk")])
    return {i.name: i for i in apply_config_to_instances(cfg, [_kernel(width)])}


def test_direct_wiring_has_no_smartconnect():
    ctx = build_hbm_smartconnect_context(_apply(), _BD())
    assert ctx["hbm_root_create"] == [] and ctx["hbm_reduce_nodes"] == []
    assert ctx["hbm_root_out"] == [{"src_pin": "k_0/m_axi_hbm", "dst_port": "HBM_AXI_12"}]


def test_plain_binding_keeps_the_smartconnect():
    ctx = build_hbm_smartconnect_context(_apply(direct=False), _BD())
    assert [r["idx"] for r in ctx["hbm_root_create"]] == [12]
    assert ctx["hbm_root_out"][0]["src_pin"] == "hbm_sc_12/M00_AXI"


def test_direct_needs_the_port_width():
    with pytest.raises(ValueError, match="256-bit"):
        _apply(width=512)


def test_direct_takes_one_master():
    with pytest.raises(ValueError, match="one master"):
        build_hbm_smartconnect_context(_apply(two=True), _BD())


def test_clock_binding_drives_the_static_region_clock():
    ctx = build_kernel_add_context(_apply())
    dst = {c["src_pin"]: c["dst"] for c in ctx["clocks"]}
    assert dst["k_0/hbm_clk"] == "[get_bd_ports static_region_clk]"
    assert dst["k_0/ap_clk"] == "[get_bd_pins user_clk]"
