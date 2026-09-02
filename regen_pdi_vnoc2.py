#!/usr/bin/env python3
"""Regenerate the static-shell PDIs for the vNOC-pinned shell.

WHY THIS IS NEEDED. promote_shell_vnoc2.sh installed the rebuilt abstract
shell (static_shell_slash.dcp / top_wrapper_routed_bb.dcp / slash_base) but
NOT the PDIs -- those come from the AVED flow, not from the Vivado shell
build. So the installed amd_v80_gen5x8_25.1*.pdi still correspond to the OLD
shell. That is fine for measuring RM timing (nothing is flashed) but means
the card cannot be programmed with the promoted combination.

This runs slashkit's own generate_base_pdi_with_aved() against the vnoc2
build directory, rather than reimplementing bootgen + build_all.sh.

It does NOT touch the card. It only produces files.

Prerequisites, all verified present before writing this:
  install.prj.vnoc2/slash.runs/impl_1/top_wrapper.bif
  install.prj.vnoc2/AVED/hw/amd_v80_gen5x8_25.1
  bootgen on PATH (source ~/dev/xilinx_setup_aved.sh)

Usage:
    python3 regen_pdi_vnoc2.py           # generate only
    python3 regen_pdi_vnoc2.py --install # also copy into slashkit resources
"""
import logging
import shutil
import sys
from pathlib import Path

SLASH = Path("/home/blaise/dev/SLASH-compute")
BUILD = SLASH / "install.prj.vnoc2"
DEST = SLASH / "linker" / "slashkit" / "resources" / "static_shell_compute"
BACKUP = SLASH / "static_shell_compute.backup-20260831"

sys.path.insert(0, str(SLASH / "linker"))
logging.basicConfig(level=logging.INFO, format="%(levelname)s %(message)s")

from slashkit.emit.hw.project_gen import generate_base_pdi_with_aved  # noqa: E402


class ShimConfig:
    """Minimal stand-in for CommandConfiguration.

    generate_base_pdi_with_aved() reads only these two attributes; passing a
    shim avoids constructing a full InstallerConfiguration (which would want
    to re-clone AVED and re-run the multi-hour Vivado build).
    """

    def __init__(self, build_dir: Path, project_name: str):
        self.build_dir = build_dir
        self.project_name = project_name


def main() -> int:
    if not BACKUP.is_dir():
        print(f"ERROR: no shell backup at {BACKUP}; refusing to overwrite PDIs.")
        return 1
    for req in (BUILD / "slash.runs" / "impl_1" / "top_wrapper.bif",
                BUILD / "AVED" / "hw" / "amd_v80_gen5x8_25.1"):
        if not req.exists():
            print(f"ERROR: missing prerequisite: {req}")
            return 1

    if shutil.which("bootgen") is None:
        print("ERROR: bootgen not on PATH -- source ~/dev/xilinx_setup_aved.sh")
        return 1

    print(f"Regenerating PDIs from {BUILD} (this does not touch the card)")
    pdi, nofpt = generate_base_pdi_with_aved(ShimConfig(BUILD, "slash_install"))
    print(f"\nGenerated:\n  {pdi}\n  {nofpt}")

    if "--install" not in sys.argv:
        print("\nNot installed. Re-run with --install to copy into:")
        print(f"  {DEST}")
        return 0

    for src in (pdi, nofpt):
        dst = DEST / src.name
        shutil.copy2(src, dst)
        print(f"installed -> {dst}")
    print("\nThe promoted shell and its PDIs are now consistent.")
    print("Flashing is a SEPARATE, MANUAL step -- this script never does it.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
