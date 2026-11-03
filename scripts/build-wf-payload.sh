#!/bin/bash
# Build one picasso test image (payload + Android boot image) from the current tree.
#
# Usage:
#   scripts/build-wf-payload.sh TAG ADDON [extra kernel args...]
#
# Environment overrides (optional, one variable per experiment):
#   OWNED_MEMORY   board.toml [payload.adt] owned_memory_size (e.g. 0x30000000)
#   DRAM_SIZE      board.toml [payload.adt] dram_size (e.g. 0x180000000)
#   DATA_RAMDISK   packed APFS Data seed (default: the current 8 GiB seed)
#   FD             firmware volume (default: uefi/SILICIUM_UEFI.fd in this tree)
#
# The kernel and the UEFI firmware are not rebuilt here. Build them first with
#   python3 unixnu build kernel prelink
# and, when the firmware sources changed,
#   python3 unixnu build firmware --uefi <Mu-Silicium> --edk2 <Mu-Silicium>/Mu_Basecore
# then copy the resulting SILICIUM_UEFI.fd to uefi/SILICIUM_UEFI.fd.
set -u
cd "$(dirname "$0")/.." || exit 1
TAG="$1"; ADDON="$2"; shift 2
DATA_RAMDISK="${DATA_RAMDISK:-build/owner-data-8g-noautodim-20261101.dbr}"
FD="${FD:-uefi/SILICIUM_UEFI.fd}"
OWNER_TRUST=/Users/silenzio/Desktop/development/ios/UniXNU/build/adreno-metal-plugin/owner-trust.bundle
BACKUP="/tmp/board.toml.${TAG}.backup"
cp board.toml "$BACKUP" || exit 1
restore() { cp "$BACKUP" board.toml; }
trap restore EXIT
python3 - "$TAG" <<'EOF' || exit 1
import os, sys
p='board.toml'
s=open(p).read()
removed=["offline-hid-swipe=16","boot-ui-focus4=liveactivitiesd","sheng-gpu-probe=1","native-ufs-diagnostics=0",
         "boot-ui-follow=1","sheng-build-tag=picasso-smp","boot-ui-focus2=SpringBoard","boot-ui-focus3=backboardd",
         "boot-ui-focus=chronod","picasso_usb_hold=0","picasso_usb_control_ms=15000"]
for t in removed:
    line='  "'+t+'",\n'
    assert line in s, t
    s=s.replace(line,'',1)
assert 'size_mib = 60\n' in s
s=s.replace('size_mib = 60\n','size_mib = 80\n',1)
import re
owned=os.environ.get('OWNED_MEMORY')
if owned:
    s, n = re.subn(r'owned_memory_size = "[^"]*"', f'owned_memory_size = "{owned}"', s, count=1)
    assert n == 1, 'owned_memory_size not found'
dram=os.environ.get('DRAM_SIZE')
if dram:
    s, n = re.subn(r'dram_size = "[^"]*"', f'dram_size = "{dram}"', s, count=1)
    assert n == 1, 'dram_size not found'
open(p,'w').write(s)
EOF
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
export UNIXNU_KERNELCACHE=/Users/silenzio/Desktop/development/ios/UniXNU/out/ipsw/21H461/kernelcache.decompressed
ARGS=(--kernel-arg sheng-disable-jobs=com.apple.driverkitd,com.apple.cameracaptured,com.apple.osanalytics.osanalyticshelper
      --kernel-arg offline-hid-start=60 --kernel-arg sheng-cpufreq=4 --kernel-arg sheng-gpu=4
      --kernel-arg sheng-gpu-metal=1 --kernel-arg sheng-gpu-addon=1 --kernel-arg sheng-display-grid=10
      --kernel-arg sheng-touch-hid=1 --kernel-arg sheng-touch=4 --kernel-arg sheng-touch-start=60)
for extra in "$@"; do ARGS+=(--kernel-arg "$extra"); done
python3 unixnu build payload --out "out/${TAG}" --kernel out/picasso/kernel/kernel.macho \
  --data-ramdisk "$DATA_RAMDISK" \
  --owner-trust-cache "$OWNER_TRUST" --gpu-addon "$ADDON" \
  --unique-chip-id 01dec0dec051a541 "${ARGS[@]}" 2>&1 | tail -3 || exit 1
python3 unixnu bootimg --uefi /Users/silenzio/Desktop/development/ios/Mu-Silicium \
  --edk2 /Users/silenzio/Desktop/development/ios/Mu-Silicium/Mu_Basecore \
  --fd "$FD" --dtb uefi/mu-silicium/Resources/DTBs/picasso.dtb \
  --ramdisk "out/${TAG}/picasso/payload/payload.img" --kernel out/picasso/kernel/kernel.macho \
  --output "out/${TAG}/picasso/boot/${TAG}.img" 2>&1 | tail -2 || exit 1
ls -l "out/${TAG}/picasso/boot/${TAG}.img"
