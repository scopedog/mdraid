#!/bin/bash
# Load md modules after a fresh boot (requires kernel-build modules).
# Usage: sudo bash tools/load-modules.sh [/path/to/raid456.ko]
#
# If no argument is given, uses /tmp/raid456.ko.
# Skips any module already loaded.

set -e

KBLD=/home/nishida/kernel-build/linux-6.12.0-124.8.1.el10_1
RAID456=${1:-/tmp/raid456.ko}

load() {
    local name=$(basename "$1" .ko)
    lsmod | grep -q "^${name} " && echo "  $name: already loaded" && return
    insmod "$1" && echo "  $name: loaded" || echo "  $name: FAILED"
}

echo "Loading md dependencies..."
load $KBLD/drivers/md/md-mod.ko
load $KBLD/crypto/xor.ko
load $KBLD/lib/raid6/raid6_pq.ko
load $KBLD/crypto/async_tx/async_tx.ko
load $KBLD/crypto/async_tx/async_memcpy.ko
load $KBLD/crypto/async_tx/async_xor.ko
load $KBLD/crypto/async_tx/async_pq.ko
load $KBLD/crypto/async_tx/async_raid6_recov.ko

echo "Loading raid456 from $RAID456..."
cp /home/nishida/mdraid-src/raid456.ko /tmp/raid456.ko
load $RAID456

# raid1 for completeness (optional)
[ -f /tmp/raid1.ko ] && load /tmp/raid1.ko || true

echo "Assembling md127..."
mdadm --assemble /dev/md127 /dev/vdc /dev/vdd /dev/vde 2>&1 || \
mdadm --assemble --force /dev/md127 /dev/vdc /dev/vdd /dev/vde 2>&1 || true

cat /proc/mdstat
