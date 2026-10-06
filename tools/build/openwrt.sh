#!/usr/bin/env bash
# Build SMIT iCast USB-C/DTMB USB Tuner through the pinned OpenWrt SDK.
set -euo pipefail
if [[ $# -ne 3 ]]; then
    echo 'usage: openwrt.sh SDK_DIRECTORY RESOURCE_CACHE OUTPUT_DIRECTORY' >&2
    exit 2
fi
repo=$(cd "$(dirname "$0")/../.." && pwd)
sdk=$(realpath "$1")
cache=$(realpath "$2")
mkdir -p "$3"
output=$(realpath "$3")
case "$sdk" in *' '*) echo 'SDK path must not contain spaces' >&2; exit 2;; esac
kernel="$sdk/build_dir/target-mipsel_24kc_musl/linux-ramips_mt7621/linux-6.12.94"
test -f "$kernel/Module.symvers"
test -f "$sdk/include/kernel.mk"
grep -q '35fb64ea09ee15c2f267bd5436b7f178' "$sdk/include/kernel.mk"
printf '%s  %s\n' \
    e998a232b9418db3301cb58468e291a4f41d6ab8306029b30d991f56251dc8d2 "$cache/linux-6.12.94.tar.xz" \
    a93d48b41067396da4875d1314017ad223d8b6fbdc51d0058546df578dd5ed39 "$kernel/.config" \
    93a8b56142ff6e182c2b48919cf85394f66d74e88f00c1d2452eb4293ca6c0bd "$kernel/Module.symvers" \
    1a0c5785c27c6b56b0d251079ed227bd54ddcdb1bdd217870454ac0b04affde8 "$kernel/include/generated/autoconf.h" | sha256sum -c -
sha256sum "$kernel/.config" "$kernel/Module.symvers" "$kernel/include/generated/autoconf.h" > "$output/kernel-before.sha256"
mkdir -p "$sdk/package/smit/src/driver" "$sdk/package/smit/src/tools/server" "$sdk/package/smit/src/include" "$sdk/dl"
cp "$repo/openwrt/smit/Makefile" "$repo/openwrt/smit/Kbuild" "$repo/openwrt/smit/dvb-usb-v2.Kbuild" "$sdk/package/smit/"
# Clear only the managed source file types so removed upstream files cannot linger.
find "$sdk/package/smit/src/driver" -maxdepth 1 -type f \( -name '*.c' -o -name '*.h' -o -name Makefile \) -delete
cp "$repo/driver/"*.c "$repo/driver/"*.h "$repo/driver/Makefile" "$sdk/package/smit/src/driver/"
find "$sdk/package/smit/src/tools/server" -maxdepth 1 -type f -delete
cp "$repo/tools/Makefile" "$sdk/package/smit/src/tools/"
cp "$repo/tools/server/"*.[ch] "$sdk/package/smit/src/tools/server/"
cp "$repo/include/"*.h "$sdk/package/smit/src/include/"
cp -r "$repo/openwrt/smit/files" "$sdk/package/smit/"
cp -n "$cache/linux-6.12.94.tar.xz" "$sdk/dl/" || true
printf '%s  %s\n' e998a232b9418db3301cb58468e291a4f41d6ab8306029b30d991f56251dc8d2 "$sdk/dl/linux-6.12.94.tar.xz" | sha256sum -c -
find "$sdk/package/smit/src" -type f -print0 | sort -z | xargs -0 sha256sum > "$output/project-source.sha256"
cd "$sdk"
new_config=0
if [[ ! -f .config ]]; then
    new_config=1
    cat > .config <<'EOF'
CONFIG_TARGET_ramips=y
CONFIG_TARGET_ramips_mt7621=y
CONFIG_TARGET_ramips_mt7621_DEVICE_xiaomi_mi-router-3g=y
# CONFIG_ALL_NONSHARED is not set
# CONFIG_ALL_KMODS is not set
# CONFIG_ALL is not set
CONFIG_PACKAGE_kmod-dvb-core=m
CONFIG_PACKAGE_kmod-dvb-usb-v2=m
CONFIG_PACKAGE_kmod-smit=m
CONFIG_PACKAGE_smit=m
EOF
fi
if [[ "$new_config" == 1 ]]; then make defconfig; fi
config_before=$(sha256sum .config | cut -d' ' -f1)
python3 - <<'PY'
from pathlib import Path
import re
path = Path('.config')
values = {'ALL': 'n', 'ALL_KMODS': 'n', 'ALL_NONSHARED': 'n', 'AUTOREMOVE': 'n',
          'PACKAGE_kmod-dvb-core': 'm', 'PACKAGE_kmod-dvb-usb-v2': 'm',
          'PACKAGE_kmod-smit': 'm', 'PACKAGE_smit': 'm'}
current = path.read_text()
expected = {key: f'# CONFIG_{key} is not set' if value == 'n' else f'CONFIG_{key}={value}'
            for key, value in values.items()}
if not all(line in current.splitlines() for line in expected.values()):
    lines = []
    for line in current.splitlines():
        match = re.match(r'(?:# )?CONFIG_([^= ]+)', line)
        if not match or match[1] not in values:
            lines.append(line)
    lines += list(expected.values())
    path.write_text('\n'.join(lines) + '\n')
PY
if [[ "$config_before" != "$(sha256sum .config | cut -d' ' -f1)" ]]; then make defconfig; fi
make package/smit/clean V=s
make -j"${JOBS:-4}" package/smit/compile V=s
sha256sum -c "$output/kernel-before.sha256"
cp .config "$output/sdk.config"
while IFS= read -r -d '' package; do cp "$package" "$output/"; done < <(
    find bin -type f \( -name 'kmod-dvb-core-[0-9]*.apk' -o -name 'kmod-dvb-usb-v2-[0-9]*.apk' -o -name 'kmod-smit-[0-9]*.apk' -o -name 'smit-[0-9]*.apk' -o -name 'libpthread-[0-9]*.apk' \) -print0
)
for name in kmod-dvb-core kmod-dvb-usb-v2 kmod-smit smit libpthread; do
    test "$(find "$output" -maxdepth 1 -name "$name-[0-9]*.apk" | wc -l)" -eq 1 || {
        echo "Expected one $name APK; use a fresh output directory" >&2; exit 1;
    }
done
sha256sum "$output/"*.apk > "$output/packages.sha256"
python3 - "$sdk" "$output" <<'PY'
import hashlib
import json
import sys
from pathlib import Path
sdk, output = map(Path, sys.argv[1:])
root = sdk / 'build_dir/target-mipsel_24kc_musl/linux-ramips_mt7621/smit-0.1/ipkg-mipsel_24kc'
payloads = []
for package in sorted(root.iterdir()):
    for path in sorted(package.rglob('*')):
        if path.is_file() and (path.suffix == '.ko' or path.relative_to(package).as_posix() == 'usr/bin/smit'):
            payloads.append({'package': package.name, 'path': '/' + str(path.relative_to(package)),
                             'bytes': path.stat().st_size,
                             'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
if len(payloads) != 4:
    raise SystemExit('Expected exactly four binary payloads')
(output / 'payloads.json').write_text(json.dumps(payloads, indent=2) + '\n')
PY
build="$sdk/build_dir/target-mipsel_24kc_musl/linux-ramips_mt7621/smit-0.1"
mkdir -p "$output/dependencies"
cp "$build/ipkg-mipsel_24kc/kmod-smit/lib/modules/6.12.94/smit.ko" "$output/smit.ko"
cp "$build/ipkg-mipsel_24kc/smit/usr/bin/smit" "$output/smit"
cp "$build/ipkg-mipsel_24kc/kmod-dvb-core/lib/modules/6.12.94/dvb-core.ko" "$output/dependencies/"
cp "$build/ipkg-mipsel_24kc/kmod-dvb-usb-v2/lib/modules/6.12.94/dvb_usb_v2.ko" "$output/dependencies/"
python3 "$repo/tools/build/write_manifest.py" "$output" openwrt-mt7621 6.12.94
