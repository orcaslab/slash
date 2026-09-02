#!/bin/bash
# Rebuild the V80 compute static shell with the 8 hbm_vnoc_* NoC ingress units
# pinned to SLR2 NMU512 sites (slash_base.tcl CONFIG.PHYSICAL_LOC).
#
# WHY. The shipped static_shell_slash.dcp was built with those PHYSICAL_LOC
# fields empty, so the NoC compiler placed 7 of the 8 in SLR1 while every HBM
# NMU is fixed in SLR2. That anchors MEM_TAG on a different die from HBM, and
# the RM inherits the split as locked NoC routes (DRC HDPR-122 refuses to move
# them afterwards). The resulting inter-SLR clock compensation is ~0.456 ns per
# path against a 0.260 ns deficit at 300 MHz.
#
# It went unnoticed because the shell is implemented with clk_wizard_slash at
# 200 MHz (top.tcl:735) where a 0.456 ns crossing is 9% of a 5 ns budget. The
# RM is then linked at 300 MHz, where it is 14% of 3.333 ns and fatal.
#
# This does NOT touch the installed shell. Artifacts land in a fresh build dir;
# promoting them is a separate, explicit step.
#
# Baseline to beat (shipped shell): clkout1_primitive_2 (200 MHz) WNS +0.103,
# clk_wizard_0_clk_out1_1 (360 MHz) WNS +0.034. If either goes negative the
# build_project.tcl timing gate should be treated as a real failure.

set -eo pipefail   # deliberately not -u: Vitis settings64 reads unset PYTHONPATH

SLASH=/home/blaise/dev/SLASH-compute
BUILD="$SLASH/install.prj.vnoc2"
SRC="$SLASH/linker/slashkit/resources/base/compute/scripts"

source ~/dev/xilinx_setup_aved.sh

mkdir -p "$BUILD/iprepo"

# Reuse the AVED checkout from the previous install instead of re-cloning.
# Only needed for downstream PDI generation, but cheap insurance.
if [ ! -d "$BUILD/AVED" ] && [ -d "$SLASH/install.prj/AVED" ]; then
  cp -a "$SLASH/install.prj/AVED" "$BUILD/AVED"
fi

# Container workaround the installer applies (project_gen.py). The bundled
# MicroBlaze cross-compiler that builds phy_ddrmc.elf aborts without it on
# Ubuntu 24.04, and silently drops the ELF.
PRELOAD=""
if [ -f /lib/x86_64-linux-gnu/libudev.so.1 ]; then
  PRELOAD=/lib/x86_64-linux-gnu/libudev.so.1
  if [ -f /lib/x86_64-linux-gnu/libcap.so.2 ]; then
    PRELOAD="$PRELOAD:/lib/x86_64-linux-gnu/libcap.so.2"
  fi
fi
if [ -n "$PRELOAD" ]; then
  export LD_PRELOAD="$PRELOAD"
fi

cd "$BUILD"
echo "BUILD DIR : $BUILD"
echo "LD_PRELOAD: ${LD_PRELOAD:-<none>}"
echo "started   : $(date)"

exec vivado -mode batch -nojournal \
  -log "$BUILD/vivado_compute.log" \
  -source "$SRC/create_project.tcl" \
  -tclargs slash_install "$BUILD/iprepo" all
