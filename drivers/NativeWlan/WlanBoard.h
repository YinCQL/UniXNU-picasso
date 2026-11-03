// SPDX-License-Identifier: BSD-2-Clause-Patent
// picasso (Qualcomm SM7250 "lito") WLAN platform facts and the pure helpers around them. Every constant states
// its source. Nothing here touches hardware; WlanProbe.cpp owns the MMIO. Host-tested by Test/main.c.
//
// The WLAN part on this board is the integrated WCSS block served over the Qualcomm "SNOC" path: there is no PCIe
// endpoint and no SDIO function for WLAN in the device tree, and the vendor qcacld module registers a SNOC bus
// driver. See WLAN_TRANSPORT_* and wlan_transport_classify() for the evidence in executable form.
#ifndef NATIVE_WLAN_BOARD_H
#define NATIVE_WLAN_BOARD_H
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------
// Transport. Determined from /tmp/picasso.dts and /tmp/picasso-vendor/stock-kernel.img:
//  * picasso.dts has no pci@/pcie node at all, and no WLAN child under either sdhci@7c4000 or sdhci@8804000
//    (picasso.dts:4907,4949). The only WLAN platform node is icnss at 0x18800000 (picasso.dts:7300).
//  * The stock kernel's icnss compatible table contains only "qcom,icnss" (stock-kernel strings, see README);
//    there is no "qcom,cnss-*" PCIe compatible. It exports icnss_qmi.c, wlfw_*_send_sync_msg and wcss_msa0; the
//    WLAN SMP2P endpoints sit under the modem's SMP2P block (remote-pid 1, picasso.dts:10878-10917).
//  * /vendor/lib/modules/qca_cld3_wlan.ko contains the SNOC host interface (hif_snoc_*, pld_snoc_probe,
//    "Fail to register snoc driver") and the BMI downloader (athwlan.bin). V12: 27 hif_snoc strings, 3 hif_pci.
//  * V13 /build.prop: ro.board.platform=lito, ro.soc.model=SM7250, ro.hardware.wlan.chip=39xx.
// Therefore the transport is the integrated WCSS/SNOC copy-engine path, not PCIe and not SDIO.
enum wlan_transport {
    WLAN_TRANSPORT_NONE = 0,
    WLAN_TRANSPORT_SNOC = 1,
    WLAN_TRANSPORT_PCIE = 2,
    WLAN_TRANSPORT_SDIO = 3,
};
#define WLAN_BOARD_TRANSPORT WLAN_TRANSPORT_SNOC

// ---------------------------------------------------------------------------------------------------------------
// icnss platform device: icnss: qcom,icnss@18800000 (picasso.dts:7300-7323).
// reg = <0x18800000 0x800000 0xb0000000 0x10000>, reg-names = "membase", "smmu_iova_ipa" (picasso.dts:7302-7303).
// membase is the WCSS register block; smmu_iova_ipa is the IOVA window used for IPA doorbells on the data path.
#define WLAN_ICNSS_BASE 0x18800000ULL
#define WLAN_ICNSS_SIZE 0x800000ULL
#define WLAN_IPA_IOVA_BASE 0xb0000000ULL
#define WLAN_IPA_IOVA_SIZE 0x10000ULL

// iommus = <0x18 0xc0 0x01> (picasso.dts:7304). Phandle 0x18 is apps_smmu: apps-smmu@15000000,
// reg = <0x15000000 0x100000 0x15182000 0x20>, #iommu-cells = <2> (picasso.dts:10710-10728). With two iommu
// cells the tuple is <phandle stream-id mask>; stream-id 0xc0, mask 0x1. The mask is the SMR ID mask (15 bits),
// so this stream matches only 0xc0. The IPA data path uses a second stream, 0x521, on ipa_smmu_wlan.
#define WLAN_APPS_SMMU_BASE 0x15000000ULL
#define WLAN_APPS_SMMU_SIZE 0x100000ULL
#define WLAN_APPS_SMMU_PHANDLE 0x18
#define WLAN_SMMU_SID 0xc0
#define WLAN_SMMU_SID_MASK 0x1
#define WLAN_IPA_SMMU_SID 0x521
// QSMMU-v500 register offsets used by the read-only plan (same layout as drivers/NativeUfs/SmmuTakeover.h).
#define WLAN_SMMU_IDR0 0x20
#define WLAN_SMMU_IDR1 0x24
#define WLAN_SMMU_IDR2 0x28
#define WLAN_SMMU_SMR_BASE 0x800
#define WLAN_SMMU_S2CR_BASE 0xc00

// icnss SMMU mapping properties (picasso.dts:7307-7309):
//   qcom,iommu-dma-addr-pool = <0xa0000000 0x10000000>  (256 MiB IOVA window)
//   qcom,iommu-dma = "fastmap"
//   qcom,iommu-faults = "stall-disable", "HUPCF", "non-fatal"
#define WLAN_IOMMU_IOVA_BASE 0xa0000000ULL
#define WLAN_IOMMU_IOVA_SIZE 0x10000000ULL

// MSA (WLAN firmware memory). qcom,wlan-msa-fixed-region = <0x99> (picasso.dts:7306); phandle 0x99 is
// pil_wlan_fw_mem: pil_wlan_fw_region@8ba00000, no-map, reg = <0x00 0x8ba00000 0x00 0x200000>
// (picasso.dts:450-454). 2 MiB, page-aligned. The vendor kernel logs "MSA pa: ... MSA Memory Size: 0x%x" and
// "Failed to translate MSA PA from device-tree" (stock-kernel strings), so the fixed region is the only MSA
// source for this board; there is no qcom,wlan-msa-memory property in this DT. The reserved-memory label says
// "pil_wlan_fw", but the only consumer is icnss's MSA property: this is the MSA pool, not a separate AP-side PIL
// image load region.
#define WLAN_MSA_BASE 0x8ba00000ULL
#define WLAN_MSA_SIZE 0x200000ULL

// Adjacent modem PIL region, for orientation only: modem_wlan_region@8c000000, 0xf800000 bytes (picasso.dts:485-489),
// phandle 0x73. pil_modem (picasso.dts:6451-6477) loads qcom,firmware-name = "modem" into this region. The WLAN
// SMP2P endpoints live under the modem's smp2p block, which indicates the WLFW service is reached over the modem
// subsystem; WlanFirmware.c records this rather than pretending the AP independently stages a wlan remoteproc.
#define WLAN_MODEM_WLAN_BASE 0x8c000000ULL
#define WLAN_MODEM_WLAN_SIZE 0xf800000ULL

// IRQs. interrupts = <0 0x84 4, 0 0x51 4, 0 0x19f 4, 0 0x1a0 4, 0 0x1a1 4, 0 0x56 4, 0 0x5e 4, 0 0x60 4,
// 0 0x52 4, 0 0x77 4, 0 0x78 4, 0 0x57 4> (picasso.dts:7305). All are GIC_SPI, level high; these are the raw
// SPI numbers (INTID = 32 + SPI). The DT gives no interrupt-names for icnss.
#define WLAN_ICNSS_IRQ_COUNT 12
extern const uint16_t wlan_icnss_irq_spi[WLAN_ICNSS_IRQ_COUNT];

// Regulators. Supply names and config properties come from the icnss node (picasso.dts:7310-7317); the rail
// definitions come from the RPMH regulator nodes. Voltage values are decimal microvolts.
//   vdd-cx-mx  -> pm8150_l1  (picasso.dts:5123-5130, phandle 0x9a): 352000..952000; config <0 0> = no override.
//   vdd-1.8-xo -> pm8150_l14 (picasso.dts:5400-5407, phandle 0x1d): 1616000..1984000; no config property.
//   vdd-1.3-rfa-> pm8150a_l2 (picasso.dts:5609-5616, phandle 0x1b): 1144000..1328000; no config property.
//   vdd-3.3-ch1-> pm8150a_l11(picasso.dts:5771-5778, phandle 0x9b): 3000000..3312000; config 3000000..3312000.
//   vdd-3.3-ch0-> pm8150a_l10(picasso.dts:5753-5760, phandle 0x1c): 3000000..3312000; config 3000000..3312000.
// The config tuple is the voltage range the vendor icnss driver applies with regulator_set_voltage(); a zero pair
// means "leave the bootloader voltage alone". This driver has no PMIC/SPMI regulator backend, so the probe only
// validates these ranges and publishes the target it would request.
struct wlan_regulator {
    const char *supply;      // DT supply property name on the icnss node
    const char *node;        // RPMH regulator label
    const char *regulator;   // regulator-name
    uint32_t min_uv, max_uv; // DT regulator-min/max-microvolt
    uint32_t cfg_min_uv, cfg_max_uv; // qcom,<supply>-config
    uint8_t has_config;      // false when the config pair is <0 0> or absent
};
#define WLAN_REGULATOR_COUNT 5
extern const struct wlan_regulator wlan_regulators[WLAN_REGULATOR_COUNT];

// Clocks and interconnect. The icnss node in this DT has no clocks, clock-names, interconnects or power-domains
// property (picasso.dts:7300-7323), so no clock or interconnect specifier can be resolved. The vendor icnss
// driver does have icnss_clk_init()/icnss_clk_deinit() and logs "Clock %s being enabled", and mentions
// "can't set frequency"; its clock list must have lived in a DT fragment that is not present in this dump.
// Writing a guessed GCC branch would risk gating a live subsystem, so the probe performs no clock writes.
#define WLAN_CLOCK_SPECIFIERS 0
#define WLAN_ICNSS_INTERCONNECTS 0

// SMP2P. qcom,smp2p_map_wlan_1_in (picasso.dts:7320-7323) maps smp2p_wlan_1_in (picasso.dts:10913-10917), whose
// entry-name is "wlan" and whose parent qcom,smp2p-mpss has remote-pid = <1> (picasso.dts:10878-10917). The two
// interrupt indices are qcom,smp2p-force-fatal-error and qcom,smp2p-early-crash-ind.
#define WLAN_SMP2P_ENTRY "wlan"
#define WLAN_SMP2P_REMOTE_PID 1
#define WLAN_SMP2P_FATAL 0
#define WLAN_SMP2P_EARLY_CRASH 1

// IPA data path. ipa_hw: qcom,ipa@1e00000, reg = <0x1e00000 0x84000 0x1e04000 0x23000>,
// reg-names = "ipa-base", "gsi-base"; qcom,ipa-hw-ver = <0x12>; qcom,ipa-wdi2; qcom,ipa-wdi2_over_gsi
// (picasso.dts:7231-7248). ipa_smmu_wlan has iommus = <0x18 0x521 0>, additional-mapping
// <0x1ea0000 0x1ea0000 0x80000> (picasso.dts:7284-7291). This is the WLAN data path once 802.11 is implemented;
// it is recorded here so the main session does not have to rediscover it.
#define WLAN_IPA_BASE 0x1e00000ULL
#define WLAN_IPA_SIZE 0x84000ULL
#define WLAN_IPA_GSI_BASE 0x1e04000ULL
#define WLAN_IPA_GSI_SIZE 0x23000ULL
#define WLAN_IPA_ADDITIONAL_PA 0x1ea0000ULL
#define WLAN_IPA_ADDITIONAL_SIZE 0x80000ULL
#define WLAN_IPA_HW_VERSION 0x12

// ---------------------------------------------------------------------------------------------------------------
// Pure helpers (implemented in WlanBoard.cpp, host-tested without hardware).
// ---------------------------------------------------------------------------------------------------------------

// Linear lookup by DT supply name; NULL if not found.
const struct wlan_regulator *wlan_regulator_find(const char *supply);

// Requested voltage for a rail: cfg_max_uv when the DT gave a non-zero config pair, otherwise 0 ("no request";
// leave the bootloader value alone).
uint32_t wlan_regulator_target_uv(const struct wlan_regulator *r);

// 0 if uv is inside the rail's DT range and its config range when one is present, -22 otherwise.
int wlan_regulator_validate(const struct wlan_regulator *r, uint32_t uv);

// Clamp uv into the intersection of the DT range and the config range. Returns 0 and stores the result, or
// -22 when the ranges are empty/absent.
int wlan_regulator_clamp(const struct wlan_regulator *r, uint32_t uv, uint32_t *out);

// Number of 4 KiB pages covered by a page-aligned MSA region; 0 for a zero, unaligned, or overflowing range.
uint64_t wlan_msa_pages(uint64_t base, uint64_t size);

// Decode one SMMU tuple from an iommus property. The cell layout is <phandle stream-id mask> because the picasso
// apps_smmu has #iommu-cells = <2>. Requires phandle == WLAN_APPS_SMMU_PHANDLE and mask <= 0x7fff; returns 0 and
// writes the stream id, or -22.
int wlan_smmu_stream_id(uint32_t phandle, uint32_t sid, uint32_t mask, uint16_t *out_sid);

// SMR value for a stream-id/mask pair, without the valid bit: (mask << 16) | (sid & 0xffff).
uint32_t wlan_smmu_smr(uint16_t sid, uint16_t mask);

// Classify the transport from board evidence. SNOC requires icnss plus a CE/SNOC host interface and no PCIe/SDIO
// WLAN endpoint; PCIe/SDIO win when such an endpoint exists.
int wlan_transport_classify(int has_icnss, int has_pci_wlan_node, int has_sdio_wlan_child, int has_ce_engine);

const char *wlan_transport_name(int transport);

#ifdef __cplusplus
}
#endif

#endif
