#!/bin/sh
# Read-only OpenWrt memory/liveness observation; send stdout to the workstation.
set -eu
duration=${1:-300}
interval=${2:-5}
case "$duration:$interval" in *[!0-9:]*|:*|*:) echo 'usage: observe.sh SECONDS INTERVAL' >&2; exit 2;; esac
[ "$duration" -gt 0 ] && [ "$interval" -gt 0 ] || exit 2
echo '=== environment ==='
date -u
uname -a
cat /proc/sys/kernel/random/boot_id
cat /proc/sys/kernel/tainted
cat /proc/modules
echo '=== initial slab ==='
cat /proc/slabinfo 2>/dev/null || true
echo '=== initial dmesg ==='
dmesg
echo '=== samples: uptime meminfo taint loadavg ==='
start=$(cut -d. -f1 /proc/uptime)
while :; do
    now=$(cut -d. -f1 /proc/uptime)
    printf 'sample uptime=%s elapsed=%s ' "$now" "$((now-start))"
    awk '/^(MemAvailable|Slab|SReclaimable|SUnreclaim|KernelStack|VmallocUsed):/ {printf "%s%s ", $1, $2}' /proc/meminfo
    printf 'taint=%s load=' "$(cat /proc/sys/kernel/tainted)"
    cat /proc/loadavg
    [ "$((now-start))" -lt "$duration" ] || break
    sleep "$interval"
done
echo '=== final slab ==='
cat /proc/slabinfo 2>/dev/null || true
echo '=== final modules ==='
cat /proc/modules
echo '=== final dmesg ==='
dmesg
echo '=== observation complete ==='
