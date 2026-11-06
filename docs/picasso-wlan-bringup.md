# Picasso WLAN bring-up

This driver does not yet scan, associate or carry packets. It performs a bounded,
read-only SMEM survey once during the existing PR13 initialization. It does not
start MPSS, alter modem NV, allocate remote SMEM items or create a network interface.

## Sources and board identity

- [UniXNU network tutorial](https://unixnu.com/porting/network): separate board,
  firmware/transport and OS network integration. Its Sheng WCN7850 PCIe/ath12k
  sequence is not the Picasso WCN399x integrated SNOC/ath10k sequence.
- [Raphael reference, aa215933](https://github.com/HeliumStudio-Dev/hackios-xnu-kernel/commit/aa215933e3e4904aa77ee82f1b7b5600d048e5a1):
  MPSS PAS 4, SMEM/SMP2P/GLINK/QRTR/QMI and ath10k SNOC. That revision reports
  successful connections but also unstable operation and failed Wi-Fi re-enable.
- Picasso stock `drivers/soc/qcom/smem.c`, `smp2p.c` and the live device tree:
  SMEM `0x80900000`, size `0x200000`; MPSS remote PID 1; incoming/outgoing SMP2P
  IDs 435/428. `WlanSmem.h` independently implements the documented wire layout.

Use Picasso's existing `WlanBoard.h`/`WlanBoard.cpp` for regulators, interrupts,
SID `0xc0` and MSA `0x8ba00000`/2 MiB. Raphael's MSA `0x8bc00000`/1.5 MiB and its
CE interrupt numbering must not replace these board values.

## Current checkpoint

`PICASSO_WLAN smem` reports SMEM v12 partition geometry and `radio_started=0`.
`PICASSO_WLAN smp2p` reports lookup/parse status for the two items and the public
`wlan` state bits. Private partition selection and global fallback follow stock
`qcom_smem_get`: use global only if the private partition is absent.

The parser bounds partition and item counts, lengths, alignment and arithmetic,
checks wire identities, and rejects changing metadata instead of waiting on it.
This is an observation without the remote allocation lock, not a transport API.
No report implies firmware or radio readiness.

Legacy `sheng-wlan=1` now reports that WCSS MMIO is deferred until firmware mission
mode; it validates only static rail descriptions. Unpowered WCSS register reads
can reset the device, as documented by the reference. Stages 2–5 remain explicit
legacy diagnostic opt-ins and are not enabled by the normal boot arguments.

## Work still required

1. Qualify Picasso MPSS signed firmware, board data and private NV inputs; add
   controlled firmware access without distributing private blobs.
2. Implement board-specific PMIC/clock ownership, secure load, shared-memory
   permissions and ordered shutdown/recovery.
3. Implement SMP2P/GLINK/QRTR/QMI and required RMTFS/TFTP/PD/PDR/DMS services, then
   prove WLFW firmware readiness and the Picasso MSA/SMMU mapping.
4. Integrate ath10k SNOC, all 12 CE interrupts, DMA/cache ownership and worker
   progress; prove scanning before attempting authentication or data traffic.
5. Connect the selected iOS Apple80211 ABI and a real interface/MAC; only then
   enable the dependent Wi-Fi launch jobs and test DHCP, DNS, traffic and rejoin.

`scripts/check-picasso-wlan.py` executes production wire readers and existing
firmware/handshake fixtures under normal and sanitizer builds, and compiles the
complete probe body for ARM64 using explicit API stand-ins. It is not an XNU SDK
build or device qualification. About-page network waits must be diagnosed through
configd/InterfaceNamer independently; enabling `wifid` alone cannot repair them.
