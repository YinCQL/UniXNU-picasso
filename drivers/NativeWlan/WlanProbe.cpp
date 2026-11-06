// SPDX-License-Identifier: BSD-2-Clause-Patent
// picasso (SM7250 "lito") WLAN bring-up probe. `sheng-wlan=<n>`:
// Always: inspect the reserved SMEM/SMP2P metadata once, without starting MPSS.
//   1  report the WCSS mission-mode prerequisite, validate the five regulator rail descriptions
//   2  read the apps-SMMU geometry and the SMR entry for stream 0xc0, compute the MSA page plan, touch MSA page 0
//   3  parse/verify a caller-supplied MBN and publish the WLFW/QMI handshake plan as far as it can be specified
//   4  publish the transport conclusion (integrated SNOC/CE; no PCIe link training or SDIO enumeration exists)
//   5  stop with the honest blocker: there is no Apple-side 802.11 stack for this Qualcomm CNSS/WCN part
// Every stage reports through native_wlan_* globals and a PICASSO_WLAN line. All work is synchronous and bounded;
// no loop waits for hardware and no stage can block the boot monitor for more than a few MMIO accesses.
// Board constants and their sources are in WlanBoard.h; the pure parsers/state machines are in WlanFirmware.c.
// Nothing here has run on hardware yet.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include <pexpert/pexpert.h>
#include <string.h>
extern "C" {
#include "WlanBoard.h"
#include "WlanFirmware.h"
#include "WlanSmem.h"
}

extern "C" {
// Boot-monitor diagnostics. The names are the contract for the main session's boot report.
volatile uint32_t native_wlan_stage_req, native_wlan_stage, native_wlan_step;
volatile int native_wlan_error;
volatile uint32_t native_wlan_transport, native_wlan_attach;
volatile uint32_t native_wlan_regs[8];
volatile uint32_t native_wlan_rails;
volatile uint32_t native_wlan_smmu_idr[3];
volatile uint32_t native_wlan_smmu_slot = 0xffffffffu;
volatile uint32_t native_wlan_smmu_smr, native_wlan_smmu_s2cr;
volatile uint32_t native_wlan_sid = WLAN_SMMU_SID;
volatile uint64_t native_wlan_msa_base = WLAN_MSA_BASE, native_wlan_msa_size = WLAN_MSA_SIZE;
volatile uint32_t native_wlan_msa_pages, native_wlan_msa_word;
volatile uint32_t native_wlan_fw_present, native_wlan_fw_segments, native_wlan_fw_hash, native_wlan_fw_payload;
volatile uint32_t native_wlan_handshake_step;
volatile int native_wlan_handshake_error;

// Entry points declared before their definitions (XNU builds with -Wmissing-prototypes).
void native_wlan_set_firmware(const uint8_t *data, size_t size);
void native_wlan_probe(void);

// Firmware is supplied by the integrator (weak symbols or the setter); no vendor blob is stored in this tree.
extern const uint8_t native_wlan_firmware[] __attribute__((weak_import));
extern const uint32_t native_wlan_firmware_size __attribute__((weak_import));
}

namespace {
static const uint8_t *gFirmware;
static size_t gFirmwareSize;

struct Region {
    OSSharedPtr<IOMemoryMap> keep;
    uintptr_t va;
};

bool mapRegion(uint64_t phys, uint64_t length, Region &region) {
    IODeviceMemory *memory = IODeviceMemory::withRange(phys, length);
    if (!memory) return false;
    region.keep = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
    memory->release();
    region.va = region.keep ? static_cast<uintptr_t>(region.keep->getVirtualAddress()) : 0;
    return region.va != 0;
}
uint32_t rd32(uintptr_t base, uint32_t reg) { return *reinterpret_cast<volatile uint32_t *>(base + reg); }

int fail(unsigned step, int code) {
    native_wlan_step = step;
    if (!native_wlan_error) native_wlan_error = code ? code : WLAN_ERR_IO;
    printf("PICASSO_WLAN fail step=%u rc=%d\n", step, code);
    return code ? code : WLAN_ERR_IO;
}

struct Session {
    Region smmu, msa;
};

// A first MPSS control-plane survey can run even with no WLAN firmware. The
// reserved SMEM mapping is read-only in our code; never read the WCSS register
// window before MPSS/WLFW reports mission mode (Raphael exposed that hazard).
static int smemRead(void *context, uint32_t offset, void *out, uint32_t size) {
    Region *region = static_cast<Region *>(context);
    volatile const uint8_t *source = reinterpret_cast<volatile const uint8_t *>(region->va);
    for (uint32_t i=0;i<size;++i) static_cast<uint8_t *>(out)[i]=source[offset+i];
    return 0;
}
static void smemSurvey() {
    Region region = {};
    if (!mapRegion(WLAN_SMEM_BASE, WLAN_SMEM_SIZE, region)) {
        printf("PICASSO_WLAN smem map error=%d\n", WLAN_ERR_NOMEM); return;
    }
    struct wlan_smem_io io = {&region, smemRead};
    struct wlan_smem_partition partition = {};
    int error = wlan_smem_partition(&io, 0, 1, &partition);
    // Match stock qcom_smem_get: global only when the private partition is absent.
    if (error == -2) error = wlan_smem_partition(&io, 0xfffe, 0xfffe, &partition);
    printf("PICASSO_WLAN smem error=%d partition=%x size=%x free=%x..%x radio_started=0\n",
        error, partition.offset, partition.size, partition.uncached, partition.cached);
    if (error) return;
    const uint16_t ids[] = {WLAN_SMEM_IN, WLAN_SMEM_OUT};
    for (unsigned i=0;i<2;++i) {
        struct wlan_smem_item item = {};
        uint32_t value=0;
        int found=wlan_smem_find(&io,&partition,ids[i],&item);
        int parsed=found ? found : wlan_smp2p_wlan(&io,&item,i ? 0 : 1,i ? 1 : 0,&value);
        printf("PICASSO_WLAN smp2p item=%u found=%d parsed=%d wlan=%x\n",ids[i],found,parsed,value);
    }
}

// Stage 1: validate static rail descriptions. No WCSS MMIO before mission mode.
int stage1(void *) {
    native_wlan_step = 11;
    // WCSS register access is not valid before the firmware enters mission mode.
    // Keep the old stage interface but report the explicit prerequisite.
    printf("PICASSO_WLAN stage1 WCSS MMIO deferred: MPSS/WLFW mission mode unavailable\n");

    native_wlan_step = 12;
    unsigned rails = 0;
    for (unsigned i = 0; i < WLAN_REGULATOR_COUNT; ++i) {
        const struct wlan_regulator *r = &wlan_regulators[i];
        const uint32_t target = wlan_regulator_target_uv(r);
        if (r->min_uv > r->max_uv) return fail(12, WLAN_ERR_INVAL);
        if (target && wlan_regulator_validate(r, target)) return fail(12, WLAN_ERR_INVAL);
        if (target) ++rails;
    }
    native_wlan_rails = rails;

    // The display/button SPMI backend does not implement WLAN rail ownership.
    // Nothing is written: see WlanBoard.h for the board's clock/interconnect facts.
    native_wlan_step = 13;
    printf("PICASSO_WLAN stage1 mmio_read=0 rails_validated=%u clocks=%u ic=%u\n",
           rails, static_cast<unsigned>(WLAN_CLOCK_SPECIFIERS),
           static_cast<unsigned>(WLAN_ICNSS_INTERCONNECTS));
    return 0;
}

// Stage 2: mapping plan. SMMU inspection is read-only; no SMR/S2CR is written. MSA page 0 is touched only after
// the page arithmetic accepts the fixed region.
int stage2(void *ctx) {
    Session *s = static_cast<Session *>(ctx);
    native_wlan_step = 21;
    if (!mapRegion(WLAN_APPS_SMMU_BASE, WLAN_APPS_SMMU_SIZE, s->smmu)) return fail(21, WLAN_ERR_NOMEM);
    const uint32_t idr0 = rd32(s->smmu.va, WLAN_SMMU_IDR0);
    const uint32_t idr2 = rd32(s->smmu.va, WLAN_SMMU_IDR2);
    native_wlan_smmu_idr[0] = idr0;
    native_wlan_smmu_idr[1] = rd32(s->smmu.va, WLAN_SMMU_IDR1);
    native_wlan_smmu_idr[2] = idr2;

    native_wlan_step = 22;
    // ARM SMMU: IDR0.SMRS(7:0) is the number of stream-match registers. Cap the scan at the 128 groups this
    // driver can name; a stream that is not mapped yet is reported as slot 0xffffffff, not treated as fatal.
    unsigned count = idr0 & 0xffu;
    if (count > 128) count = 128;
    uint32_t slot = 0xffffffffu, smr = 0, s2cr = 0;
    unsigned matches = 0;
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t candidate = rd32(s->smmu.va, WLAN_SMMU_SMR_BASE + 4 * i);
        if (!(candidate & 0x80000000u)) continue;
        if ((candidate & 0xffffu) != WLAN_SMMU_SID) continue;
        ++matches;
        if (slot == 0xffffffffu) {
            slot = i;
            smr = candidate;
            s2cr = rd32(s->smmu.va, WLAN_SMMU_S2CR_BASE + 4 * i);
        }
    }
    native_wlan_smmu_slot = slot;
    native_wlan_smmu_smr = smr;
    native_wlan_smmu_s2cr = s2cr;

    native_wlan_step = 23;
    const uint64_t pages = wlan_msa_pages(WLAN_MSA_BASE, WLAN_MSA_SIZE);
    if (!pages) return fail(23, WLAN_ERR_INVAL);
    native_wlan_msa_pages = static_cast<uint32_t>(pages);
    if (!mapRegion(WLAN_MSA_BASE, 0x1000, s->msa)) return fail(23, WLAN_ERR_NOMEM);
    native_wlan_msa_word = rd32(s->msa.va, 0);

    printf("PICASSO_WLAN stage2 idr=%08x/%08x/%08x sid=%x slot=%u matches=%u smr=%08x s2cr=%08x "
           "msa=%llx pages=%u word=%08x\n",
           native_wlan_smmu_idr[0], native_wlan_smmu_idr[1], native_wlan_smmu_idr[2], native_wlan_sid,
           native_wlan_smmu_slot, matches, native_wlan_smmu_smr, native_wlan_smmu_s2cr,
           (unsigned long long)native_wlan_msa_base, native_wlan_msa_pages, native_wlan_msa_word);
    return 0;
}

// Stage 3: firmware. A staged MBN is parsed and hash-checked; the WLFW request order is published. The actual
// PIL/SCM load is deliberately not faked: this tree has no secure-monitor call, and the picasso DT has no wlan PIL
// node, so the load step reports ENOTSUP. A missing or unloadable image is a diagnostic here, not a crash; the
// final stop belongs to stage 5.
struct HandshakeContext { int unused; };

int hsLoad(void *, const struct wlan_mbn *) {
    printf("PICASSO_WLAN stage3 PIL load unavailable: no SCM/TZ call in this tree and no wlan PIL node in "
           "picasso.dts (modem.mdt/modem_wlan_mem is the only staged modem image)\n");
    return WLAN_ERR_NOTSUP;
}
int hsRequest(void *, unsigned request, uint64_t, uint64_t) {
    printf("PICASSO_WLAN stage3 WLFW request %u skipped: no QMI/GLINK client\n", request);
    return WLAN_ERR_NOTSUP;
}
int hsWait(void *, unsigned indication, unsigned) {
    printf("PICASSO_WLAN stage3 WLFW indication %u skipped: no QMI/GLINK client\n", indication);
    return WLAN_ERR_NOTSUP;
}

int stage3(void *ctx) {
    (void)ctx;
    native_wlan_step = 31;
    size_t size = 0;
    const uint8_t *firmware = gFirmware && gFirmwareSize ? gFirmware : nullptr;
    if (!firmware && &native_wlan_firmware && &native_wlan_firmware_size && native_wlan_firmware_size) {
        firmware = native_wlan_firmware;
        size = native_wlan_firmware_size;
    } else if (firmware) {
        size = gFirmwareSize;
    }
    if (!firmware || !size) {
        native_wlan_fw_present = 0;
        native_wlan_handshake_step = WLAN_HS_LOAD;
        native_wlan_handshake_error = WLAN_ERR_NOENT;
        printf("PICASSO_WLAN stage3 no firmware staged: link native_wlan_firmware[]/native_wlan_firmware_size or "
               "call native_wlan_set_firmware() with the device's MBN (wlanmdsp/athwlan are not in this tree)\n");
        return 0;
    }
    native_wlan_fw_present = 1;
    native_wlan_step = 32;
    struct wlan_mbn mbn;
    int rc = wlan_mbn_parse(&mbn, firmware, size);
    if (rc) return fail(32, rc);
    rc = wlan_mbn_verify_hashes(&mbn, firmware, size);
    if (rc) return fail(32, rc);
    native_wlan_fw_segments = mbn.segments;
    native_wlan_fw_hash = mbn.hash_index == WLAN_MBN_NO_HASH ? 0 : mbn.hash_count;
    native_wlan_fw_payload = static_cast<uint32_t>(mbn.payload_size);

    native_wlan_step = 33;
    struct HandshakeContext hctx = {0};
    const struct wlan_handshake_ops ops = {&hctx, hsLoad, hsRequest, hsWait};
    struct wlan_handshake_state state;
    rc = wlan_handshake_run(&ops, &mbn, WLAN_MSA_BASE, WLAN_MSA_SIZE, 50, &state);
    native_wlan_handshake_step = state.step;
    native_wlan_handshake_error = state.error;
    printf("PICASSO_WLAN stage3 fw=%u bytes segs=%u hash=%u payload=%u load=not-run handshake_step=%u error=%d\n",
           static_cast<unsigned>(size), native_wlan_fw_segments, native_wlan_fw_hash, native_wlan_fw_payload,
           native_wlan_handshake_step, native_wlan_handshake_error);
    // A malformed image is fatal; an unavailable secure load is an environment fact reported by stage 5.
    return 0;
}

// Stage 4: transport. The evidence says integrated SNOC/CE, so there is no PCIe link to train and no SDIO
// function to enumerate. Listing the conclusion explicitly keeps the absence of an attach from looking like a bug.
int stage4(void *) {
    native_wlan_step = 41;
    const int transport = wlan_transport_classify(1, 0, 0, 1);
    native_wlan_transport = static_cast<uint32_t>(transport);
    native_wlan_attach = 0;   // 0 = not applicable
    if (transport != WLAN_BOARD_TRANSPORT) return fail(41, WLAN_ERR_INVAL);
    printf("PICASSO_WLAN stage4 transport=%s attach=none (no PCIe node, no SDIO WLAN child; integrated WCSS/SNOC)\n",
           wlan_transport_name(transport));
    return 0;
}

// Stage 5: the honest stop. There is no IO80211/mac80211 replacement in this tree and no WLAN data path.
int stage5(void *) {
    native_wlan_step = 51;
    printf("PICASSO_WLAN stage5 stop: no Apple 802.11 stack (IO80211) or mac80211-style data path for this "
           "Qualcomm CNSS/WCN part; WLAN cannot be brought up by this driver\n");
    return WLAN_ERR_NOTSUP;
}

void copyState(const struct wlan_probe_state &state) {
    native_wlan_stage = state.stage;
    native_wlan_step = state.step;
    native_wlan_error = state.error;
}
}  // namespace

extern "C" void native_wlan_set_firmware(const uint8_t *data, size_t size) {
    gFirmwareSize = size;
    __atomic_store_n(&gFirmware, data, __ATOMIC_RELEASE);
}

// Callable from the boot monitor. Always performs one bounded SMEM survey;
// `sheng-wlan=<n>` selects additional legacy diagnostic stages. The entry point
// depends only on IOKit and its own pure helpers, so a boot monitor can declare it
// weak_import and link whether or not this driver is included.
extern "C" void native_wlan_probe(void) {
    static bool surveyed;
    if (!surveyed) { surveyed=true; smemSurvey(); }
    unsigned stage = 0;
    if (!PE_parse_boot_argn("sheng-wlan", &stage, sizeof stage) || stage < 1) return;
    if (native_wlan_stage_req) return;
    if (stage > WLAN_PROBE_STAGE_MAX) stage = WLAN_PROBE_STAGE_MAX;
    native_wlan_stage_req = stage;

    Session session = {};
    const struct wlan_probe_ops ops = {&session, stage1, stage2, stage3, stage4, stage5};
    struct wlan_probe_state state;
    const int rc = wlan_probe_run(&ops, stage, &state);
    copyState(state);
    printf("PICASSO_WLAN end stage=%u step=%u error=%d transport=%s\n", native_wlan_stage, native_wlan_step,
           native_wlan_error, wlan_transport_name(static_cast<int>(native_wlan_transport)));
    (void)rc;
}
