// SPDX-License-Identifier: BSD-2-Clause-Patent
// Pure board helpers for NativeWlan. No IOKit, no MMIO: WlanProbe.cpp supplies the hardware side and Test/main.c
// supplies the host checks. Compiled as C++ in the kernel; the ABI is C because WlanBoard.h wraps every
// declaration in extern "C".
#include "WlanBoard.h"

// Raw SPI numbers from picasso.dts:7305, in DT order. The GIC INTID is 32 + SPI.
const uint16_t wlan_icnss_irq_spi[WLAN_ICNSS_IRQ_COUNT] = {
    0x084, 0x051, 0x19f, 0x1a0, 0x1a1, 0x056,
    0x05e, 0x060, 0x052, 0x077, 0x078, 0x057,
};

// Voltage values are decimal microvolts converted from the hex in picasso.dts (see WlanBoard.h for line numbers).
const struct wlan_regulator wlan_regulators[WLAN_REGULATOR_COUNT] = {
    {"vdd-cx-mx",   "pm8150_l1",   "pm8150_l1",   352000,  952000,       0,       0, 0},
    {"vdd-1.8-xo",  "pm8150_l14",  "pm8150_l14",  1616000, 1984000,      0,       0, 0},
    {"vdd-1.3-rfa", "pm8150a_l2",  "pm8150a_l2",  1144000, 1328000,      0,       0, 0},
    {"vdd-3.3-ch1", "pm8150a_l11", "pm8150a_l11", 3000000, 3312000, 3000000, 3312000, 1},
    {"vdd-3.3-ch0", "pm8150a_l10", "pm8150a_l10", 3000000, 3312000, 3000000, 3312000, 1},
};

const struct wlan_regulator *wlan_regulator_find(const char *supply) {
    if (!supply) return nullptr;
    for (unsigned i = 0; i < WLAN_REGULATOR_COUNT; ++i) {
        const char *a = wlan_regulators[i].supply;
        const char *b = supply;
        while (*a && *a == *b) { ++a; ++b; }
        if (!*a && !*b) return &wlan_regulators[i];
    }
    return nullptr;
}

uint32_t wlan_regulator_target_uv(const struct wlan_regulator *r) {
    if (!r) return 0;
    return r->has_config ? r->cfg_max_uv : 0;   // 0 means "no request"; see WlanBoard.h
}

int wlan_regulator_validate(const struct wlan_regulator *r, uint32_t uv) {
    if (!r || uv < r->min_uv || uv > r->max_uv) return -22;
    if (r->has_config && (uv < r->cfg_min_uv || uv > r->cfg_max_uv)) return -22;
    return 0;
}

int wlan_regulator_clamp(const struct wlan_regulator *r, uint32_t uv, uint32_t *out) {
    if (!r || !out) return -22;
    uint32_t lo = r->min_uv, hi = r->max_uv;
    if (r->has_config) {
        if (r->cfg_min_uv > lo) lo = r->cfg_min_uv;
        if (r->cfg_max_uv < hi) hi = r->cfg_max_uv;
    }
    if (lo > hi) return -22;
    *out = uv < lo ? lo : uv > hi ? hi : uv;
    return 0;
}

uint64_t wlan_msa_pages(uint64_t base, uint64_t size) {
    const uint64_t page = 0x1000;
    if (!size || (base & (page - 1)) || (size & (page - 1))) return 0;
    if (base > ~0ULL - size) return 0;
    return size / page;
}

int wlan_smmu_stream_id(uint32_t phandle, uint32_t sid, uint32_t mask, uint16_t *out_sid) {
    if (phandle != WLAN_APPS_SMMU_PHANDLE) return -22;
    // #iommu-cells = 2 means the third cell is an SMR ID mask; bits 15..31 are not encodable.
    if (mask > 0x7fffu || sid > 0xffffu) return -22;
    if (out_sid) *out_sid = (uint16_t)sid;
    return 0;
}

uint32_t wlan_smmu_smr(uint16_t sid, uint16_t mask) {
    return ((uint32_t)mask << 16) | (uint32_t)sid;
}

int wlan_transport_classify(int has_icnss, int has_pci_wlan_node, int has_sdio_wlan_child, int has_ce_engine) {
    if (has_pci_wlan_node) return WLAN_TRANSPORT_PCIE;
    if (has_sdio_wlan_child) return WLAN_TRANSPORT_SDIO;
    if (has_icnss && has_ce_engine) return WLAN_TRANSPORT_SNOC;
    return WLAN_TRANSPORT_NONE;
}

const char *wlan_transport_name(int transport) {
    switch (transport) {
        case WLAN_TRANSPORT_SNOC: return "snoc";
        case WLAN_TRANSPORT_PCIE: return "pcie";
        case WLAN_TRANSPORT_SDIO: return "sdio";
        default: return "none";
    }
}
