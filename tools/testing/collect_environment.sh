#!/usr/bin/env bash
# Read-only host/device inspection. No module loads or vendor USB commands.
set -u
umask 077
out=${1:-"environment-$(date -u +%Y%m%dT%H%M%SZ)"}
mkdir -- "$out" || exit 1
out=$(cd -- "$out" && pwd)
run() {
    local name=$1
    shift
    { printf 'command:'; printf ' %q' "$@"; printf '\n';
      "$@"; result=$?; printf '\nexit: %s\n' "$result";
    } >"$out/$name.txt" 2>&1
}
krel=$(uname -r)
kdir=${KDIR:-/lib/modules/$krel/build}
run identity bash -c 'date -u; uname -a; cat /etc/os-release; id; systemd-detect-virt'
run tools bash -c 'set -o pipefail; for cmd in gcc gcc-15 make ld readelf modinfo lsusb mokutil; do echo "tool: $cmd"; command -v "$cmd"; "$cmd" --version 2>&1 | head -n 3; echo "tool_exit: ${PIPESTATUS[0]}"; done'
run packages dpkg-query -W '-f=${binary:Package}\t${db:Status-Abbrev}\t${Version}\n' 'linux-image*' 'linux-headers*' 'linux-modules*' 'linux-source*' 'gcc*' 'make' 'binutils'
run build-path bash -c 'ls -ld "$1"; readlink -f "$1"; cat "$1/include/config/kernel.release"; ls -l "$1/Module.symvers" "$1/include/generated/autoconf.h"; cat "$1/include/generated/compile.h"' _ "$kdir"
run config bash -c 'if test -r "/boot/config-$1"; then cat "/boot/config-$1"; elif test -r /proc/config.gz; then zcat /proc/config.gz; else echo "Kernel config unavailable"; exit 1; fi' _ "$krel"
run dvb-config bash -c 'grep -E "^(CONFIG_(MEDIA_SUPPORT|MEDIA_USB_SUPPORT|DVB_CORE|DVB_USB_V2|MODULES|MODVERSIONS|MODULE_SIG[^=]*|SECURITY_LOCKDOWN[^=]*)=|# CONFIG_DVB_USB_V2)" "$1/config.txt"' _ "$out"
run v2-modinfo modinfo -k "$krel" dvb_usb_v2
run v2-symbols bash -c 'grep -E "[[:space:]](dvb_usbv2_|dvb_register_adapter|dvb_register_frontend|dvb_dmx_).*[[:space:]]" "$1/Module.symvers"' _ "$kdir"
run private-headers bash -c 'for dir in "$1" /usr/src; do find -L "$dir" -name dvb_usb.h -o -name usb_urb.h 2>/dev/null; done' _ "$kdir"
run secure-boot mokutil --sb-state
run lockdown cat /sys/kernel/security/lockdown
run signature-enforcement cat /sys/module/module/parameters/sig_enforce
run modules lsmod
run usb-list lsusb
run usb-descriptors lsusb -v -d 29df:0001
run usb-tree lsusb -t
run usb-sysfs bash -c '
found=0
for dev in /sys/bus/usb/devices/*; do
    test -r "$dev/idVendor" && test -r "$dev/idProduct" || continue
    test "$(cat "$dev/idVendor"):$(cat "$dev/idProduct")" = 29df:0001 || continue
    found=1
    echo "device: $dev"
    for f in idVendor idProduct bcdDevice speed busnum devnum devpath manufacturer product serial bConfigurationValue; do
        test ! -r "$dev/$f" || { printf "%s: " "$f"; cat "$dev/$f"; }
    done
    for intf in "$dev"/*:*; do
        test -d "$intf" || continue
        echo "interface: $intf"; readlink "$intf/driver"
        for ep in "$intf"/ep_*; do
            test -d "$ep" || continue
            echo "endpoint: $ep"
            for f in bEndpointAddress bmAttributes wMaxPacketSize bInterval type direction; do
                test ! -r "$ep/$f" || { printf "%s: " "$f"; cat "$ep/$f"; }
            done
        done
    done
done
test "$found" = 1'
run dmesg dmesg --ctime
run kernel-journal journalctl -k -b --no-pager
printf 'Collected: %s\nInspect each exit status; collection does not certify readiness.\n' "$out"
