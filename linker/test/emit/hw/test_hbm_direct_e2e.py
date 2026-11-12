# ##################################################################################################
#  The MIT License (MIT)
#  Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
#  (See the repository's LICENSE.)
# ##################################################################################################

"""End to end, short of Vivado: a kernel with N direct HBM masters (256 bits, its HBM side in a clock
bound to the static region clock) linked against the compute shell (the shell the Foldsim kernel
uses). The connectivity file is parsed, the instances bound, and the whole slash Tcl rendered by
generate_tcl; the rendered Tcl must wire each master straight to its HBM_AXI port (no SmartConnect,
no terminator on it), drive the HBM-side clock from static_region_clk and the control clock from
user_clk, and connect nothing to the unused HBM ports (slashkit leaves unused HBM_AXI ports open, as
in every image without HBM bindings). N = 32 (one master a pseudo-channel), 54 (two a
pseudo-channel on PCs 5-31) and 64 (every port)."""

import re
from pathlib import Path
import pytest

from slashkit.core.command_config import LinkerConfiguration, ShellType, Platform
from slashkit.core.bd_ports import load_bd_ports_from_lines, generate_bd_port_lines
from slashkit.core.kernel import Kernel
from slashkit.core.port import BusType, Port
from slashkit.parser.config_parser import parse_connectivity_file, apply_config_to_instances
from slashkit.emit.hw.tcl_gen import generate_tcl


def _kernel(n):
    ports = {"ap_clk": Port("ap_clk", BusType.CLOCK), "hbm_clk": Port("hbm_clk", BusType.CLOCK),
             "ap_rst_n": Port("ap_rst_n", BusType.RESET),
             "m_axi_mem_0": Port("m_axi_mem_0", BusType.AXI4FULL, width=512)}
    for k in range(n):
        ports[f"m_axi_hbm_{k}"] = Port(f"m_axi_hbm_{k}", BusType.AXI4FULL, width=256)
    return Kernel(name="foldsim_afu", component_xml_path=Path("component.xml"), ports=ports,
                  vlnv="vortex:gpgpu:foldsim_afu:1.0")


def _channels(n):
    if n == 32:
        return list(range(0, 64, 2))              # one port a pseudo-channel (its even port)
    if n == 54:
        return list(range(10, 64))                # both ports of PCs 5-31
    return list(range(64))


def _link(tmp_path, n, direct=True):
    hbm = _channels(n)
    lines = ["[connectivity]", "shell=compute", "nk=foldsim_afu:1:foldsim_afu_0",
             "sp=foldsim_afu_0.m_axi_mem_0:MEM", "clk=foldsim_afu_0.hbm_clk:STATIC"]
    lines += [f"sp=foldsim_afu_0.m_axi_hbm_{k}:HBM{h}" + (":DIRECT" if direct else "") for k, h in enumerate(hbm)]
    cfg_file = tmp_path / "link.cfg"
    cfg_file.write_text("\n".join(lines) + "\n")
    cfg = parse_connectivity_file(cfg_file)
    kernel = _kernel(n)
    conf = LinkerConfiguration.__new__(LinkerConfiguration)
    conf._configuration = cfg
    conf._kernels = [kernel]
    conf._kernel_instances = apply_config_to_instances(cfg, [kernel])
    conf._bd_ports = load_bd_ports_from_lines(generate_bd_port_lines(num_ddr=12, num_virt=0))
    conf._build_dir = tmp_path
    conf._project_name = "vport_direct"
    conf._clock_hz = 200_000_000
    conf._platform = Platform.HARDWARE if hasattr(Platform, "HARDWARE") else Platform("hw")
    assert conf.shell_type == ShellType.COMPUTE
    generate_tcl(conf)
    return (tmp_path / "slash.tcl").read_text(), hbm


def _joined(tcl):
    return re.sub(r"\\\n\s*", " ", tcl)


@pytest.mark.parametrize("n", [32, 54, 64])
def test_direct_ports_render(tmp_path, n):
    tcl, hbm = _link(tmp_path, n)
    t = _joined(tcl)
    for k, h in enumerate(hbm):
        assert re.search(rf"connect_bd_intf_net\s+\[get_bd_intf_pins foldsim_afu_0/m_axi_hbm_{k}\]\s+"
                         rf"\[get_bd_intf_ports HBM_AXI_{h:02d}\]", t), f"m_axi_hbm_{k} -> HBM_AXI_{h:02d}"
    # No SmartConnect and no terminator on a directly bound port.
    assert not re.search(r"create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 hbm_sc_", t)
    for h in hbm:
        assert not re.search(rf"\[get_bd_intf_ports HBM_AXI_{h:02d}\]\s*$", "\n".join(
            l for l in t.split("\n") if "axi_term" in l or "terminator" in l.lower()), re.M)
    # Nothing is connected to the unused HBM ports.
    for h in range(64):
        if h not in hbm:
            assert not re.search(rf"connect_bd_intf_net[^\n]*HBM_AXI_{h:02d}\]", t), f"HBM_AXI_{h:02d} connected"
    # Clocks: the HBM side from the static region clock, the control side from user_clk.
    assert re.search(r"connect_bd_net \[get_bd_pins foldsim_afu_0/hbm_clk\] \[get_bd_ports static_region_clk\]", t)
    assert re.search(r"connect_bd_net \[get_bd_pins foldsim_afu_0/ap_clk\] \[get_bd_pins user_clk\]", t)


def test_plain_binding_keeps_a_smartconnect_per_channel(tmp_path):
    tcl, hbm = _link(tmp_path, 32, direct=False)
    t = _joined(tcl)
    assert len(re.findall(r"xilinx.com:ip:smartconnect:1.0 hbm_sc_\d+", t)) == len(hbm)


def test_two_masters_on_a_direct_channel_fail(tmp_path):
    cfg = tmp_path / "bad.cfg"
    cfg.write_text("[connectivity]\nshell=compute\nnk=foldsim_afu:1:foldsim_afu_0\n"
                   "sp=foldsim_afu_0.m_axi_hbm_0:HBM12:DIRECT\nsp=foldsim_afu_0.m_axi_hbm_1:HBM12:DIRECT\n")
    c = parse_connectivity_file(cfg)
    conf = LinkerConfiguration.__new__(LinkerConfiguration)
    kernel = _kernel(2)
    conf._configuration = c
    conf._kernels = [kernel]
    conf._kernel_instances = apply_config_to_instances(c, [kernel])
    conf._bd_ports = load_bd_ports_from_lines(generate_bd_port_lines(num_ddr=12, num_virt=0))
    conf._build_dir = tmp_path
    conf._project_name = "bad"
    conf._clock_hz = 200_000_000
    with pytest.raises(ValueError, match="one master"):
        generate_tcl(conf)


def test_foldsim_flow_cfg(tmp_path):
    """The cfg Foldsim's flow writes for a kernel with 32 direct static ports (foldsim_flow.py cfg, [platform]
    hbm_direct: m_axi_hbm_<p> on HBM channel 2p, hbm_clk on STATIC), with the kernel's interfaces as its top has them."""
    src = Path(__file__).resolve().parents[2] / "fixtures" / "foldsim_direct" / "link.cfg"
    cfg = parse_connectivity_file(src)
    conf = LinkerConfiguration.__new__(LinkerConfiguration)
    kernel = _kernel(32)
    conf._configuration = cfg
    conf._kernels = [kernel]
    conf._kernel_instances = apply_config_to_instances(cfg, [kernel])
    conf._bd_ports = load_bd_ports_from_lines(generate_bd_port_lines(num_ddr=12, num_virt=0))
    conf._build_dir = tmp_path
    conf._project_name = "foldsim_direct"
    conf._clock_hz = 200_000_000
    generate_tcl(conf)
    t = _joined((tmp_path / "slash.tcl").read_text())
    for k in range(32):
        assert re.search(rf"connect_bd_intf_net\s+\[get_bd_intf_pins foldsim_afu_0/m_axi_hbm_{k}\]\s+"
                         rf"\[get_bd_intf_ports HBM_AXI_{2 * k:02d}\]", t)
    assert not re.search(r"smartconnect:1.0 hbm_sc_", t)
    assert re.search(r"connect_bd_net \[get_bd_pins foldsim_afu_0/hbm_clk\] \[get_bd_ports static_region_clk\]", t)
