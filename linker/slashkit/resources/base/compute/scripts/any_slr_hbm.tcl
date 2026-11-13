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
#
#  Per-SLR HBM ingress for the compute shell.
#
#  Every NOC_NMU_HBM2E site on xcv80 is in SLR2, so the 64 direct HBM ports (HBM_AXI_*) and the
#  eight MEM vNoC ports (HBM_VNOC_INI_*, pinned to SLR2 NMU512 sites) all enter the NoC in SLR2.
#  User logic in SLR0 or SLR1 that talks to HBM therefore crosses dies on SLL fabric.
#
#  This file adds NoC ingress units (NMU512) in SLR0 and SLR1, inside pblock_slash, each with its
#  own INI boundary port that reaches all 32 HBM pseudo-channels (the same 32 GB window as MEM).
#  HBM traffic from those SLRs then crosses dies inside the NoC (VNoC + inter-die bridges).
#
#  The NoC solution of a reconfigurable module is locked to the sites this shell chooses (DRC
#  HDPR-122), so these ports can only be created here, not by an RM.
#
#  Two entry points, called from the generated BD scripts just before their validate_bd_design:
#    asl_rp      -- inside slash_base.bd (the RP): ports, NoC cells, default-RM traffic generators
#    asl_static  -- inside top.bd: static NoC inputs on axi_noc_cips, hierarchy pins, QoS, addresses
# ##################################################################################################

# One row per new port: {slr index site}. Index is per SLR. Sites are NMU512 sites inside
# pblock_slash that the shipped shell leaves free (see the any-SLR HBM proposal, section 2).
# NMU512 SLR bands on xcv80: SLR0 rows Y0-Y6, SLR1 rows Y7-Y12, SLR2 rows Y13-Y18.
#
# SLR0: the RP owns exactly 8 NMU512 sites there (rows Y5-Y6), all free in the shipped shell.
# SLR1: 15 of 21 RP sites are free; two per VNoC column are used, matching SLR0.
# SLR2: no new ports. MEM (HBM_VNOC_INI_00..07) already enters there, plus the 64 HBM_AXI ports.
# Two ports per VNoC column and SLR = one per physical VNoC channel (8 channels and 8 inter-die
# bridges per SLR boundary on xcv80).
if {![info exists ::asl_ports]} {
  set ::asl_ports {
    {0 0 NOC_NMU512_X0Y5} {0 1 NOC_NMU512_X0Y6}
    {0 2 NOC_NMU512_X1Y5} {0 3 NOC_NMU512_X1Y6}
    {0 4 NOC_NMU512_X2Y5} {0 5 NOC_NMU512_X2Y6}
    {0 6 NOC_NMU512_X3Y5} {0 7 NOC_NMU512_X3Y6}
    {1 0 NOC_NMU512_X0Y8} {1 1 NOC_NMU512_X0Y10}
    {1 2 NOC_NMU512_X1Y7} {1 3 NOC_NMU512_X1Y8}
    {1 4 NOC_NMU512_X2Y8} {1 5 NOC_NMU512_X2Y9}
    {1 6 NOC_NMU512_X3Y8} {1 7 NOC_NMU512_X3Y10}
  }
}
# Bandwidth reserved per new port (MB/s, each direction). The static side spreads it evenly over
# the 32 pseudo-channels.
if {![info exists ::asl_bw]} {
  set ::asl_bw 4000
}
# First axi_noc_cips NoC slave input used for the new ports (the shipped shell uses S00..S20).
set ::asl_nsi_base 21

# HBM pseudo-channel address map (same as HBM_VNOC_INI_*): HBM<k>_PC<p> at 0x40_0000_0000 + (2k+p) GB.
proc asl_pc_segments {} {
  set segs {}
  for {set k 0} {$k < 16} {incr k} {
    for {set p 0} {$p < 2} {incr p} {
      lappend segs [list HBM${k}_PC${p} [expr {0x4000000000 + (2 * $k + $p) * 0x40000000}]]
    }
  }
  return $segs
}

proc asl_name {slr idx} { return [format "hbm_s%d_noc_%02d" $slr $idx] }
proc asl_port {slr idx} { return [format "HBM_S%d_INI_%02d" $slr $idx] }
proc asl_tg   {slr idx} { return [format "hbm_s%d_tg_%02d" $slr $idx] }

proc asl_rp {} {
  if {[llength $::asl_ports] == 0} { return }
  set n [llength $::asl_ports]
  set apertures [list]
  foreach s [asl_pc_segments] { lappend apertures [list [format "0x%X" [lindex $s 1]] 1G] }

  # AXI-Lite for the traffic generators: one more SmartConnect hung off the end of the chain.
  set sc5 [get_bd_cells smartconnect_5]
  set mi5 [get_property CONFIG.NUM_MI $sc5]
  set_property CONFIG.NUM_MI [expr {$mi5 + 1}] $sc5
  set sc6 [create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 smartconnect_asl]
  set_property -dict [list CONFIG.NUM_CLKS {1} CONFIG.NUM_MI $n CONFIG.NUM_SI {1}] $sc6
  connect_bd_intf_net [get_bd_intf_pins smartconnect_5/M[format %02d $mi5]_AXI] [get_bd_intf_pins smartconnect_asl/S00_AXI]
  connect_bd_net [get_bd_ports user_clk] [get_bd_pins smartconnect_asl/aclk]
  connect_bd_net [get_bd_pins ilreduced_logic_0/Res] [get_bd_pins smartconnect_asl/aresetn]

  set i 0
  foreach row $::asl_ports {
    lassign $row slr idx site
    set nm [asl_name $slr $idx]
    set pt [asl_port $slr $idx]
    set tg [asl_tg $slr $idx]

    set port [create_bd_intf_port -mode Master -vlnv xilinx.com:interface:inimm_rtl:1.0 $pt]
    set_property CONFIG.INI_STRATEGY {driver} $port
    set_property APERTURES $apertures $port

    set noc [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_noc:1.1 $nm]
    set_property -dict [list CONFIG.NSI_NAMES {} CONFIG.NUM_MI {0} CONFIG.NUM_NMI {1} CONFIG.NUM_NSI {0}] $noc
    set_property CONFIG.INI_STRATEGY {driver} [get_bd_intf_pins /$nm/M00_INI]
    set_property -dict [list \
      CONFIG.PHYSICAL_LOC $site \
      CONFIG.CONNECTIONS [list M00_INI [list read_bw $::asl_bw write_bw $::asl_bw]] \
      CONFIG.NOC_PARAMS {} \
      CONFIG.CATEGORY {pl} \
    ] [get_bd_intf_pins /$nm/S00_AXI]
    set_property CONFIG.ASSOCIATED_BUSIF {S00_AXI} [get_bd_pins /$nm/aclk0]
    connect_bd_intf_net [get_bd_intf_ports $pt] [get_bd_intf_pins $nm/M00_INI]
    connect_bd_net [get_bd_ports user_clk] [get_bd_pins $nm/aclk0]

    # Default-RM traffic generator, as for every other memory port of this shell.
    set t [create_bd_cell -type ip -vlnv xilinx.com:hls:hbm_bandwidth:1.0 $tg]
    set_property CONFIG.C_M_AXI_GMEM0_DATA_WIDTH {256} $t
    connect_bd_intf_net [get_bd_intf_pins $tg/m_axi_gmem0] [get_bd_intf_pins $nm/S00_AXI]
    connect_bd_intf_net [get_bd_intf_pins smartconnect_asl/M[format %02d $i]_AXI] [get_bd_intf_pins $tg/s_axi_control]
    connect_bd_net [get_bd_ports user_clk] [get_bd_pins $tg/ap_clk]
    connect_bd_net [get_bd_pins ilreduced_logic_0/Res] [get_bd_pins $tg/ap_rst_n]
    assign_bd_address -offset 0x004000000000 -range 0x000800000000 \
      -target_address_space [get_bd_addr_spaces $tg/Data_m_axi_gmem0] [get_bd_addr_segs $pt/Reg] -force
    incr i
  }
  # Control registers of the new generators: next free slots in the S_AXILITE_INI window.
  assign_bd_address
}

proc asl_static {} {
  if {[llength $::asl_ports] == 0} { return }
  set n [llength $::asl_ports]
  set cips [get_bd_cells /static_region/noc/axi_noc_cips]
  set nsi0 [get_property CONFIG.NUM_NSI $cips]
  if {$nsi0 != $::asl_nsi_base} {
    error "any_slr_hbm: axi_noc_cips has NUM_NSI=$nsi0, expected $::asl_nsi_base"
  }
  set_property CONFIG.NUM_NSI [expr {$nsi0 + $n}] $cips
  set per_pc [expr {int(ceil(double($::asl_bw) / 32.0))}]

  set i 0
  foreach row $::asl_ports {
    lassign $row slr idx site
    set pt  [asl_port $slr $idx]
    set nsi [format "S%02d_INI" [expr {$::asl_nsi_base + $i}]]
    # Alternate the HBM NSU ports per INI, as the MEM vNoC ports do (even: PORT0/2, odd: PORT1/3).
    set off [expr {$i % 2}]
    set conns [list]
    for {set k 0} {$k < 16} {incr k} {
      foreach p [list $off [expr {$off + 2}]] {
        lappend conns HBM${k}_PORT${p} [list read_bw $per_pc write_bw $per_pc read_avg_burst 64 write_avg_burst 64 initial_boot false]
      }
    }
    set_property -dict [list CONFIG.INI_STRATEGY {auto} CONFIG.CONNECTIONS $conns] \
      [get_bd_intf_pins /static_region/noc/axi_noc_cips/$nsi]

    create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 /static_region/noc/$pt
    create_bd_intf_pin -mode Slave -vlnv xilinx.com:interface:inimm_rtl:1.0 /static_region/$pt
    connect_bd_intf_net [get_bd_intf_pins /static_region/noc/axi_noc_cips/$nsi] [get_bd_intf_pins /static_region/noc/$pt]
    connect_bd_intf_net [get_bd_intf_pins /static_region/noc/$pt] [get_bd_intf_pins /static_region/$pt]
    connect_bd_intf_net [get_bd_intf_pins /slash/$pt] [get_bd_intf_pins /static_region/$pt]

    set tg [asl_tg $slr $idx]
    foreach s [asl_pc_segments] {
      lassign $s seg off
      assign_bd_address -offset [format "0x%012X" $off] -range 0x40000000 \
        -target_address_space [get_bd_addr_spaces slash/$tg/Data_m_axi_gmem0] \
        [get_bd_addr_segs static_region/noc/axi_noc_cips/$nsi/$seg] -force
    }
    incr i
  }
}
