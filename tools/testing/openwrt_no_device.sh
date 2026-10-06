#!/bin/sh
# Requires the three matching DVB packages. Never run with a receiver attached.
set -eu
cycles=${1:-100}
case "$cycles" in ''|*[!0-9]*) exit 2;; esac
[ "$cycles" -gt 0 ] && [ "$cycles" -le 1000 ] || exit 2
assert_no_receiver() {
    for path in /sys/bus/usb/devices/*; do
        [ -r "$path/idVendor" ] && [ -r "$path/idProduct" ] || continue
        if [ "$(cat "$path/idVendor"):$(cat "$path/idProduct")" = '29df:0001' ]; then
            echo 'STOP: SMIT receiver is attached' >&2
            exit 1
        fi
    done
    for path in /sys/bus/usb/drivers/smit/*:*; do
        [ ! -L "$path" ] || { echo 'STOP: SMIT interface is bound' >&2; exit 1; }
    done
}
sample() {
    printf 'cycle=%s uptime=%s ' "$1" "$(cut -d. -f1 /proc/uptime)"
    awk '/^(MemAvailable|Slab|SReclaimable|SUnreclaim|KernelStack|VmallocUsed):/ {printf "%s%s ", $1, $2}' /proc/meminfo
    printf 'taint=%s\n' "$(cat /proc/sys/kernel/tainted)"
}
assert_no_receiver
echo '=== initial dmesg ==='
dmesg
modprobe smit
initial_taint=$(cat /proc/sys/kernel/tainted)
sample loaded
round=1
total=$((cycles+10))
while [ "$round" -le "$total" ]; do
    assert_no_receiver
    rmmod smit
    [ ! -d /sys/module/smit ]
    modprobe smit
    [ -d /sys/module/smit ]
    [ ! -e /dev/dvb/adapter0/frontend0 ]
    [ "$(cat /proc/sys/kernel/tainted)" = "$initial_taint" ] || {
        echo 'STOP: kernel taint changed; inspect dmesg' >&2
        exit 1
    }
    if dmesg | grep -Eq 'BUG:|Oops:|Kernel panic|Out of memory:|oom-kill:|WARNING: CPU:|Unknown symbol'; then
        echo 'STOP: kernel error candidate; inspect dmesg and baseline' >&2
        exit 1
    fi
    if [ "$round" -le 10 ]; then sample "warmup-$round"; else sample "$((round-10))"; fi
    round=$((round+1))
    sleep 1
done
assert_no_receiver
rmmod smit
sleep 60
sample unloaded-settled
modprobe smit
echo '=== final modules ==='
cat /proc/modules
echo '=== final dmesg ==='
dmesg
echo '=== no-device cycles complete ==='
