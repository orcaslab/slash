#!/bin/bash
# Install SLASH build dependencies (Ubuntu 24.04). Run: sudo bash slash_deps.sh
# Tolerant of the pre-existing broken nvidia packages: verifies each package
# individually rather than trusting apt's overall exit code.
export DEBIAN_FRONTEND=noninteractive

PKGS=(cmake pkg-config ninja-build libxml2-dev libzmq3-dev libjsoncpp-dev \
      zlib1g-dev libsystemd-dev libinih-dev libcli11-dev \
      "linux-headers-$(uname -r)")

echo "==> apt-get install ${PKGS[*]}"
apt-get install -y "${PKGS[@]}" || echo "(apt returned non-zero; verifying packages individually below)"

echo "==> Verification:"
fail=0
for p in "${PKGS[@]}"; do
  if dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q 'install ok installed'; then
    printf '  %-32s OK\n' "$p"
  else
    printf '  %-32s MISSING\n' "$p"; fail=1
  fi
done
[ "$fail" = 0 ] && echo "==> All SLASH build dependencies present." \
                || echo "==> Some packages missing (likely blocked by the broken nvidia state)."
