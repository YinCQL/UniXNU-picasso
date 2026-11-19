# Source attribution

The locked `btqca.c` and `btqca.h` originate from
[ianchb/sm8550-mainline](https://github.com/ianchb/sm8550-mainline), commit
`1fb56e66091e71160d7c7f8e13b065d5b6ab8b8c`. The generated copies retain their
original notices and GPL-2.0-only license.

GENI register and DMA sequences follow `qcom_geni_serial.c`, `qcom-geni-se.c`
and `geni-se.h` at that same revision. Their identities are recorded in
`tools/source-lock.json`.

`src/qbt_transport.c` derives from
[Renoir XNU Bluetooth](https://github.com/daimile2/renoir_xnu_bluethooth/tree/2b9d59c921c9ec803462b2be8deb6980f7297c22),
`kernel/files/bsd/dev/renoir_bluetooth_transport.inc`, by daimile2 and
contributors. Its APSL-2.0 notice accompanies the file. The WCN7850 adapter
adds channel lifetime coordination, power completion requests, range checks,
queue copies and nonfatal packet allocation handling.

The sheng board references are
[sm8550-mainline](https://github.com/ianchb/sm8550-mainline) and
[debian-sheng](https://github.com/ianchb/debian-sheng).

Picasso's protocol and DMA adapter are based on UniXNU/QcaBluetooth
22ff97f93416452335bb61badb14c762da653487. The board integration and SMMU
planner derive from UniXNU/UniXNU-sheng ce0c600fa015964d4ecb6af864fa2ab66efa915f.
The three userland sources and native advertising regression derive from
daimile2/renoir_xnu_bluethooth 2b9d59c921c9ec803462b2be8deb6980f7297c22
(BSD-3-Clause). Picasso uses WCN399x, ordinary arm64, and iOS 16 build 20H392.
The original Linux files in linux/ remain byte-identical; the local adapter
adds version-envelope selection and rejected-segment error propagation.

The private userland Skywalk channel and packet implementations are compiled
from the same prepared Apple XNU xnu-10063.121.3 sources and headers as the
kernel. Their APSL-2.0 and BSD notices remain in the generated sources. The
bluetoothd and BTAudioHALPlugin private entry points retain the native schema
and metadata checks. The exact iOS 16.7.16/20H392 BTAudioHALPlugin is adapted
offline and retains its original UUID, API behavior and Info.plist.
