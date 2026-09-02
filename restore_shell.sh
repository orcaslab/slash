#!/bin/bash
# Put the shipped static shell back, undoing promote_shell_vnoc2.sh.
set -eo pipefail

SLASH=/home/blaise/dev/SLASH-compute
DEST="$SLASH/linker/slashkit/resources/static_shell_compute"
BACKUP="$SLASH/static_shell_compute.backup-20260831"

[ -d "$BACKUP" ] || { echo "RESTORE: no backup at $BACKUP" >&2; exit 1; }

echo "RESTORE: restoring $BACKUP -> $DEST"
rm -rf "$DEST"
cp -a "$BACKUP" "$DEST"
echo "RESTORE: done."
ls -la "$DEST"
