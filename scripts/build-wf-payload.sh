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
#   GRID=0         omit sheng-display-grid=10 (room for extra kernel args)
#   SEED_PREFS=1   run the Data seed transform: Payload --skip-setup writes the
#                  UIKit Pad_ForcedNoCell provider pref (and the inject library).
#                  This changes the seed identity and drops the persistent store.
#   SB_INJECT=1    carry picasso_inject_sb=1 and amfi_allow_any_signature=1 (the
#                  kernel also writes the pref and copies the library, so the seed
#                  stays untouched); trims -v and the
#                  osanalytics helper from the disabled-job list to stay under
#                  1000 bytes
#   TRIM_ARGS=1    trim -v without the injection pair (room for extra arguments).
#                  picasso-aks-state=1 is a boot dependency and stays present.
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
OWNER_TRUST="${OWNER_TRUST:-/Users/silenzio/Desktop/development/ios/UniXNU/build/adreno-metal-plugin/owner-trust.bundle}"
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
if os.environ.get('SB_INJECT') == '1' or os.environ.get('TRIM_ARGS') == '1':
    # Room for picasso_inject_sb=1/amfi_allow_any_signature=1 or sheng-staged-apps=1 under 1000 bytes.
    # -v only controls boot verbosity. picasso-aks-state=1 also supplies the
    # working AKS state path and must survive command-line trimming.
    if os.environ.get('KEEP_V') != '1':
        removed += ["-v"]
for t in removed:
    line='  "'+t+'",\n'
    assert line in s, t
    s=s.replace(line,'',1)
assert 'size_mib = 60\n' in s
s=s.replace('size_mib = 60\n','size_mib = 88\n',1)
import re
owned=os.environ.get('OWNED_MEMORY')
if owned:
    s, n = re.subn(r'owned_memory_size = "[^"]*"', f'owned_memory_size = "{owned}"', s, count=1)
    assert n == 1, 'owned_memory_size not found'
dram=os.environ.get('DRAM_SIZE')
if dram:
    s, n = re.subn(r'dram_size = "[^"]*"', f'dram_size = "{dram}"', s, count=1)
    assert n == 1, 'dram_size not found'
if os.environ.get('DATA_PERSIST') is not None:
    # 0 = pristine RAM Data every boot (clears the persistent MobileGestalt cache and any other Data writes),
    # 1 = read-only persistent window, 2 = read/write (board default).
    line = '  "sheng-data-persist=2",\n'
    assert line in s, 'sheng-data-persist=2 missing'
    s = s.replace(line, f'  "sheng-data-persist={os.environ["DATA_PERSIST"]}",\n', 1)
if os.environ.get('NO_OFFLINE_HID') == '1':
    for line in ('  "offline-hid-input=1",\n', '  "offline-hid-home=1",\n'):
        assert line in s, line.strip()
        s = s.replace(line, '', 1)
if os.environ.get('NO_HIDHOME') == '1':
    # DANGER: package/HidInputProbe.cpp creates the HID digitizer device (the path real Novatek touch
    # reports go through) only when offline-hid-home=1 is set. Removing it disables real touch entirely
    # (wf155/wf158: "PICASSO_HID stage=3 ... meta=-1 object=0", no "event driver ready").
    line = '  "offline-hid-home=1",\n'
    assert line in s, 'offline-hid-home=1 missing'
    s = s.replace(line, '', 1)
if os.environ.get('NO_FBGPU') == '1':
    line = '  "sheng-fb-gpu=1",\n'
    assert line in s, 'sheng-fb-gpu=1 missing'
    s = s.replace(line, '', 1)

open(p,'w').write(s)
EOF
if [ "${IPHONE_IDENTITY:-0}" = "1" ]; then export PICASSO_PHONE_IDENTITY=1; fi
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
export UNIXNU_KERNELCACHE=/Users/silenzio/Desktop/development/ios/UniXNU/out/ipsw/21H461/kernelcache.decompressed
# Permanent board defaults (cpufreq, GPU probe/metal, HID start, touch stages, vsync, the
# offline-legacy display chain) are compiled into the drivers; only arguments a second consumer
# (the kernel addon mount, the register probe) or an experiment still needs travel here.
ARGS=(--kernel-arg sheng-gpu-addon=1)
if [ "${GRID:-1}" != "0" ]; then ARGS+=(--kernel-arg sheng-display-grid=10); fi
SKIP_SETUP=()
if [ "${SEED_PREFS:-0}" = "1" ]; then
  # The seed transform is cached by base hash and literal arguments, not by the provider prefs' content.
  rm -f out/picasso/data-skip-setup-*.dbr out/picasso/data-skip-setup-*.json
  SKIP_SETUP=(--skip-setup)
fi
if [ "${SB_INJECT:-0}" = "1" ]; then
  # The two injection arguments cost 49 bytes. The kernel's Data preparation already writes the provider pref and
  # copies the library from the addon, so --skip-setup (and the store reset its seed change causes) is not needed.
  # Drop the analytics helper from the disabled-job list (it is a diagnostic, not a boot dependency) for room.
  ARGS+=(--kernel-arg picasso_inject_sb=1)
  if [ "${SB_INJECT_NO_AMFI:-0}" != "1" ]; then
    ARGS+=(--kernel-arg amfi_allow_any_signature=1)
  fi
fi
for extra in "$@"; do ARGS+=(--kernel-arg "$extra"); done
python3 unixnu build payload --out "out/${TAG}" --kernel out/picasso/kernel/kernel.macho \
  --data-ramdisk "$DATA_RAMDISK" "${SKIP_SETUP[@]}" \
  --owner-trust-cache "$OWNER_TRUST" --gpu-addon "$ADDON" \
  --unique-chip-id "${UNIQUE_CHIP_ID:-01dec0dec051a541}" "${ARGS[@]}" 2>&1 | tail -3 || exit 1
python3 unixnu bootimg --uefi /Users/silenzio/Desktop/development/ios/Mu-Silicium \
  --edk2 /Users/silenzio/Desktop/development/ios/Mu-Silicium/Mu_Basecore \
  --fd "$FD" --dtb uefi/mu-silicium/Resources/DTBs/picasso.dtb \
  --ramdisk "out/${TAG}/picasso/payload/payload.img" --kernel out/picasso/kernel/kernel.macho \
  --output "out/${TAG}/picasso/boot/${TAG}.img" 2>&1 | tail -2 || exit 1
ls -l "out/${TAG}/picasso/boot/${TAG}.img"
