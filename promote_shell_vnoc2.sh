#!/bin/bash
# Promote the vNOC-pinned static shell from install.prj.vnoc2 into the resource
# directory that slashkit actually loads, so an RM build links against it.
#
# slashkit resolves the shell as a package resource. /home/blaise/.local/bin/slashkit
# does sys.path.insert(0,"/home/blaise/dev/SLASH-compute/linker"), so the live
# copy is linker/slashkit/resources/static_shell_compute -- NOT the root-owned
# /usr/lib/python3.12/dist-packages copy, which is shadowed. No sudo needed.
#
# DESTRUCTIVE: overwrites the working shell. Restore with restore_shell.sh.
#
# NOTE ON PDIs. This does not regenerate amd_v80_gen5x8_25.1*.pdi -- those come
# from the AVED flow and are only needed to FLASH the static shell. The existing
# PDIs will no longer correspond to the promoted abstract shell, which is fine
# for measuring RM timing (nothing is flashed) but means the card must NOT be
# reprogrammed with this combination.

set -eo pipefail

SLASH=/home/blaise/dev/SLASH-compute
BUILD="$SLASH/install.prj.vnoc2"
DEST="$SLASH/linker/slashkit/resources/static_shell_compute"
BACKUP="$SLASH/static_shell_compute.backup-20260831"

IMPL="$BUILD/slash.runs/impl_1"
BD_SRC="$BUILD/slash.srcs/sources_1/bd/slash_base"

fail() { echo "PROMOTE: $*" >&2; exit 1; }

[ -d "$BACKUP" ] || fail "no backup at $BACKUP -- refusing to overwrite the working shell"

for f in "$IMPL/static_shell_slash.dcp" "$IMPL/top_wrapper_routed_bb.dcp"; do
  [ -f "$f" ] || fail "missing build artifact: $f"
done
[ -d "$BD_SRC" ] || fail "missing base BD directory: $BD_SRC"

# Refuse to promote a shell that failed its own timing. The shipped baseline is
# WNS +0.034 (360 MHz clk_wizard_0) and +0.103 (200 MHz clk_wizard_slash).
RPT="$BUILD/report_timing_slash_install.txt"
if [ -f "$RPT" ]; then
  if ! grep -q "All user specified timing constraints are met" "$RPT"; then
    echo "PROMOTE: WARNING -- the rebuilt shell does NOT meet its own timing."
    echo "PROMOTE: see $RPT"
    if [ "${FORCE_PROMOTE:-0}" != "1" ]; then
      fail "refusing to promote a shell that failed timing (set FORCE_PROMOTE=1 to override)"
    fi
    echo "PROMOTE: FORCE_PROMOTE=1 set, continuing anyway."
  fi
else
  echo "PROMOTE: WARNING -- no timing report found at $RPT"
fi

echo "PROMOTE: installing new shell into $DEST"
cp -f "$IMPL/static_shell_slash.dcp"     "$DEST/static_shell_slash.dcp"
cp -f "$IMPL/top_wrapper_routed_bb.dcp"  "$DEST/top_wrapper_routed_bb.dcp"

rm -rf "$DEST/slash_base"
cp -a "$BD_SRC" "$DEST/slash_base"
# resources.path() needs package markers. The installer's _add_init_files()
# touches __init__.py in the BD dir AND recursively in every subdirectory
# (181 of them, one per IP). Only the top level leaves the tree inconsistent
# with a real install.
find "$DEST/slash_base" -type d | while read -r d; do touch "$d/__init__.py"; done

# stale bytecode can pin old resource paths
rm -rf "$DEST/__pycache__"

echo "PROMOTE: done."
ls -la "$DEST"
