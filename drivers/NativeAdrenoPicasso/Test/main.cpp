// SPDX-License-Identifier: MIT
// Host tests for the picasso Adreno 620 board inputs, firmware containers and
// staged probe. No hardware is touched. Synthetic images reproduce the vendor
// container shapes; when ADRENO620_VENDOR_DIR points at the extracted vendor
// files, the same parsers also validate the real blobs.
#include "Adreno620Board.h"
#include "Adreno620Firmware.h"
#include "Adreno620ProbeCore.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define CHECK(c) do { if (!(c)) { \
    std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

using namespace adreno620;

static void put16(std::vector<uint8_t> &d, size_t at, uint16_t v)
{
    d[at] = static_cast<uint8_t>(v);
    d[at + 1] = static_cast<uint8_t>(v >> 8);
}

static void put32(std::vector<uint8_t> &d, size_t at, uint32_t v)
{
    d[at] = static_cast<uint8_t>(v);
    d[at + 1] = static_cast<uint8_t>(v >> 8);
    d[at + 2] = static_cast<uint8_t>(v >> 16);
    d[at + 3] = static_cast<uint8_t>(v >> 24);
}

static std::vector<uint8_t> readFile(const std::string &path)
{
    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) {
        std::printf("FAIL cannot open %s\n", path.c_str());
        std::exit(1);
    }
    CHECK(std::fseek(file, 0, SEEK_END) == 0);
    const long length = std::ftell(file);
    CHECK(length >= 0);
    CHECK(std::fseek(file, 0, SEEK_SET) == 0);
    std::vector<uint8_t> data(static_cast<size_t>(length));
    if (!data.empty())
        CHECK(std::fread(data.data(), 1, data.size(), file) == data.size());
    std::fclose(file);
    return data;
}

// ---------------------------------------------------------------------------
// CRC32 and descriptor checks
// ---------------------------------------------------------------------------
static void testCrc()
{
    CHECK(Adreno620Crc32("123456789", 9) == 0xcbf43926u);
    CHECK(Adreno620Crc32("x", 0) == 0u);
    const uint8_t bytes[4] = {0xde, 0xad, 0xbe, 0xef};
    const uint32_t crc = Adreno620Crc32(bytes, sizeof(bytes));
    CHECK(Adreno620Crc32Update(Adreno620Crc32Update(0, bytes, 2), bytes + 2, 2)
        == crc);
    CHECK(Adreno620FirmwareCheck(bytes, sizeof(bytes), sizeof(bytes), crc) == 1);
    CHECK(Adreno620FirmwareCheck(bytes, sizeof(bytes), sizeof(bytes) + 1, crc) == 0);
    CHECK(Adreno620FirmwareCheck(bytes, sizeof(bytes), sizeof(bytes), crc ^ 1) == 0);
}

// ---------------------------------------------------------------------------
// Device-tree parsing and clock arithmetic
// ---------------------------------------------------------------------------
static void testDt()
{
    // 2/2-cell big-endian window: <0x0 0x3d00000 0x0 0x40000>.
    const uint8_t be[16] = {
        0, 0, 0, 0, 0x03, 0xd0, 0, 0,
        0, 0, 0, 0, 0x00, 0x04, 0, 0,
    };
    Window out[2]{};
    size_t count = 0;
    CHECK(parseReg(be, sizeof(be), 2, 2, out, 2, &count) == ParseOk);
    CHECK(count == 1);
    CHECK(out[0].base == 0x3d00000u && out[0].bytes == 0x40000u);
    // The ADT 64-bit little-endian quirk.
    const uint8_t le[16] = {
        0, 0, 0xd0, 0x03, 0, 0, 0, 0,
        0, 0, 0x04, 0x00, 0, 0, 0, 0,
    };
    CHECK(parseReg(le, sizeof(le), 2, 2, out, 2, &count) == ParseOk);
    CHECK(count == 1);
    CHECK(out[0].base == 0x3d00000u && out[0].bytes == 0x40000u);
    // 1/1 cells and failure cases.
    const uint8_t one[8] = {0x03, 0xd6, 0xa0, 0x00, 0x00, 0x03, 0x00, 0x00};
    CHECK(parseReg(one, sizeof(one), 1, 1, out, 2, &count) == ParseOk);
    CHECK(count == 1 && out[0].base == 0x3d6a000u && out[0].bytes == 0x30000u);
    const uint8_t zero[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(parseReg(zero, sizeof(zero), 1, 1, out, 2, &count) == ParseBadArgument);
    CHECK(parseReg(be, sizeof(be), 3, 2, out, 2, &count) == ParseBadArgument);
    CHECK(parseReg(be, sizeof(be) - 1, 2, 2, out, 2, &count) == ParseBadArgument);
    CHECK(parseReg(be, sizeof(be), 2, 2, out, 0, &count) == ParseNoSpace);

    // gfx3d_user <&kgsl_smmu 0x0 0x401>: phandle, SID, SMR mask.
    const uint8_t iommu[12] = {
        0, 0, 0, 0xa7, 0, 0, 0, 0x00, 0, 0, 0x04, 0x01,
    };
    IommuEntry entries[4]{};
    CHECK(parseIommus(iommu, sizeof(iommu), 2, entries, 4, &count) == ParseOk);
    CHECK(count == 1 && entries[0].sid == 0 && entries[0].mask == 0x401);
    const uint8_t bad_phandle[12] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x04, 0x01,
    };
    CHECK(parseIommus(bad_phandle, sizeof(bad_phandle), 2, entries, 4, &count)
        == ParseBadArgument);
    CHECK(parseIommus(iommu, sizeof(iommu) - 1, 2, entries, 4, &count)
        == ParseBadArgument);

    const uint16_t gfx_user[4] = {0, 1, 0x400, 0x401};
    uint16_t sids[16]{};
    CHECK(expandStreamMask(0, 0x401, sids, 16) == 4);
    for (size_t i = 0; i < 4; ++i) CHECK(sids[i] == gfx_user[i]);
    CHECK(expandStreamMask(2, 0x400, sids, 16) == 2);
    CHECK(sids[0] == 2 && sids[1] == 0x402);
    CHECK(expandStreamMask(5, 0x400, sids, 16) == 2);
    CHECK(sids[0] == 5 && sids[1] == 0x405);
    CHECK(expandStreamMask(0, 0x1f, sids, 16) == 0); // more than four mask bits

    // Clock divider arithmetic: Qualcomm RCG output = parent / (2*(m+1)).
    CHECK(rcgRateHz(1500000000u, 0) == 750000000u);
    CHECK(rcgRateHz(1500000000u, 1) == 375000000u);
    CHECK(rcgRateHz(19200000u, 0) == 9600000u);
    uint32_t m = 99;
    CHECK(rcgMForRate(1500000000u, 750000000u, 15, &m) == 0 && m == 0);
    CHECK(rcgMForRate(1500000000u, 500000000u, 15, &m) == -1);
    CHECK(rcgMForRate(1500000000u, 375000000u, 15, &m) == 0 && m == 1);
    CHECK(rcgMForRate(1500000000u, 400000000u, 15, &m) == -1);
    CHECK(rcgMForRate(1500000000u, 750000000u, 0, &m) == 0 && m == 0);
    CHECK(usToTicks(19200000u, 1000) == 19200u);
    CHECK(usToTicks(19200000u, 1) == 20u);
    CHECK(usToTicks(0, 1) == 0u);
    CHECK(ticksToUs(19200u, 19200000u) == 1000u);
}

static void testBoard()
{
    size_t count = 0;
    const Window *windows = adreno620::windows(&count);
    CHECK(windows && count == WindowCount);
    CHECK(window(WindowGpu).base == 0x3d00000u && window(WindowGpu).bytes == 0x40000u);
    CHECK(window(WindowGmu).base == 0x3d6a000u && window(WindowGmu).bytes == 0x30000u);
    CHECK(window(WindowSmmu).base == 0x3da0000u && window(WindowSmmu).bytes == 0x10000u);
    CHECK(window(WindowGpucc).base == 0x3d90000u && window(WindowGpucc).bytes == 0x9000u);
    CHECK(gicIntId(interrupt(InterruptGpu).spi) == 332u);
    CHECK(interrupt(InterruptHfi).spi == 0x130u);
    CHECK(interrupt(InterruptGmu).spi == 0x131u);
    CHECK(clock(ClockGpuccCxGmu).controller == ClockController::Gpucc);
    CHECK(clock(ClockGpuccCxGmu).id == 3u);
    CHECK(clock(ClockGpuccSmmuVote).id == 12u);
    CHECK(clock(ClockGccGpuMemnocGfx).id == 35u);
    CHECK(ChipId == 0x06020001u);

    const PwrlevelBin &bin = defaultPwrlevelBin();
    CHECK(bin.speed_bin == 0 && bin.count == 7);
    CHECK(bin.levels[0].frequency_hz == 750000000u);
    CHECK(bin.levels[6].frequency_hz == 0u);
    CHECK(pwrlevelBinForSpeed(0x84) != nullptr);
    CHECK(pwrlevelBinForSpeed(0x84)->count == 5);
    CHECK(pwrlevelBinForSpeed(0x73)->count == 4);
    CHECK(pwrlevelBinForSpeed(0x1234) == nullptr);
    CHECK(gpuBusLevelCount() == 13);
    CHECK(gpuBusKbps(1) == 400000u && gpuBusKbps(12) == 8371200u);
    CHECK(gpuBusKbps(13) == 0u);
    CHECK(gmuCnocKbps(1) == 100u && smmuBusKbps(1) == 1000u);
    CHECK(gpuBusPath().src == 0x1a && gpuBusPath().dst == 0x200);
    CHECK(gmuCnocPath().dst == 0x2734 && smmuBusPath().src == 0x9b);
    const NvmemCell *fuses = nvmemCells(&count);
    CHECK(fuses && count == 3);
    CHECK(fuses[0].address == 0x1eau && fuses[0].bit == 5 && fuses[0].bits == 8);
    CHECK(fuses[1].address == 0x212u && fuses[1].bits == 1);
    CHECK(fuses[2].address == 0x45c8u && fuses[2].bits == 32);

    const SmmuStream *streams = smmuStreams(&count);
    CHECK(streams && count == 4);
    CHECK(streams[0].sid == 0 && streams[0].mask == 0x401);
    CHECK(streams[3].sid == 5 && streams[3].mask == 0x400);
    const SmmuBank *banks = smmuBanks(&count);
    CHECK(banks && count == 4);
    CHECK(banks[0].bank == 0 && banks[0].asid == 1 && banks[0].bringup);
    CHECK(banks[3].bank == 3 && banks[3].asid == 4 && banks[3].bringup);
    CHECK(smmuAsidForBank(0) == 1u && smmuAsidForBank(3) == 4u);
    CHECK(smmu().iommu_cells == 2u && smmu().global_interrupts == 1u);
    CHECK(smmu().actlr_value == 0x32bu);

    CHECK(zapSegmentCount() == 3);
    CHECK(std::strcmp(zapSegment(0).name, "a620_zap.b00") == 0);
    CHECK(zapMdt().bytes == 6860u && zapMdt().crc32 == 0x7de1dd05u);
    CHECK(sqe().bytes == 32264u && sqe().crc32 == 0xf9a2972eu);
    CHECK(sqeAlternate() && sqeAlternate()->bytes == 31632u);
    CHECK(gmu().bytes == 41548u && gmu().crc32 == 0x5c66275cu);
    CHECK(ZapEntry == 0x5000u && ZapCodeVaddr == 0x5000u &&
        ZapHashVaddr == 0x6000u);
    CHECK(SqeIovaAlignment == 0x1000u && GmuEntry == 0u);
    const GmuLayout &layout = gmuLayout();
    CHECK(layout.icache_iova == 0x4000u && layout.icache_bytes == 0xffc000u);
    CHECK(layout.dtcm_base == 0x10004000u);
    CHECK(layout.dummy_bytes == 0x1000u);
    CHECK(layout.log_iova == 0x60001000u && layout.hfi_iova == 0x60005000u);
}

// ---------------------------------------------------------------------------
// Synthetic MDT/ELF, matching the vendor layout
// ---------------------------------------------------------------------------
struct MdtParts {
    std::vector<uint8_t> mdt;
    std::vector<uint8_t> b00, b01, b02;
    std::vector<uint8_t> elf;
};

static void putPhdr(std::vector<uint8_t> &d, size_t at, uint32_t type,
    uint32_t offset, uint32_t vaddr, uint32_t paddr, uint32_t filesz,
    uint32_t memsz, uint32_t flags, uint32_t align)
{
    put32(d, at + 0, type);
    put32(d, at + 4, offset);
    put32(d, at + 8, vaddr);
    put32(d, at + 12, paddr);
    put32(d, at + 16, filesz);
    put32(d, at + 20, memsz);
    put32(d, at + 24, flags);
    put32(d, at + 28, align);
}

static MdtParts makeMdt()
{
    MdtParts parts;
    parts.mdt.assign(0x94 + 0x1a38, 0);
    std::vector<uint8_t> &mdt = parts.mdt;
    mdt[0] = 0x7f; mdt[1] = 'E'; mdt[2] = 'L'; mdt[3] = 'F';
    mdt[4] = 1;    // ELFCLASS32
    mdt[5] = 1;    // ELFDATA2LSB
    mdt[6] = 1;    // EV_CURRENT
    put16(mdt, 0x10, 2);      // ET_EXEC
    put16(mdt, 0x12, 0xa4);   // EM_QCOM
    put32(mdt, 0x14, 1);
    put32(mdt, 0x18, 0x5000); // entry
    put32(mdt, 0x1c, 0x34);   // phoff
    put16(mdt, 0x28, 0x34);   // ehsize
    put16(mdt, 0x2a, 0x20);   // phentsize
    put16(mdt, 0x2c, 3);      // phnum
    putPhdr(mdt, 0x34, 0, 0, 0, 0, 0x94, 0, 0x07000000, 0);
    putPhdr(mdt, 0x54, 0, 0x1000, 0x6000, 0x6000, 0x1a38, 0x2000,
        0x02200000, 0x1000);
    putPhdr(mdt, 0x74, 1, 0x3000, 0x5000, 0x5000, 0x68c, 0x68c,
        0x08000007, 0x100000);
    for (size_t i = 0x94; i < mdt.size(); ++i) mdt[i] = 0x22;

    parts.b00.assign(0x94, 0x11);
    parts.b01.assign(0x1a38, 0x22);
    parts.b02.assign(0x68c, 0x33);
    parts.elf.assign(0x368c, 0);
    std::memcpy(parts.elf.data(), mdt.data(), 0x94);
    std::memcpy(parts.elf.data() + 0x1000, parts.b01.data(), parts.b01.size());
    std::memcpy(parts.elf.data() + 0x3000, parts.b02.data(), parts.b02.size());
    return parts;
}

static Adreno620FwStatus parseMdt(const MdtParts &parts, Adreno620MdtImage &out)
{
    const uint8_t *segments[3] = {
        parts.b00.data(), parts.b01.data(), parts.b02.data(),
    };
    const size_t sizes[3] = {
        parts.b00.size(), parts.b01.size(), parts.b02.size(),
    };
    return Adreno620MdtParse(parts.mdt.data(), parts.mdt.size(), segments,
        sizes, 3, &out);
}

static void testMdt()
{
    MdtParts parts = makeMdt();
    Adreno620MdtImage image;
    CHECK(parseMdt(parts, image) == Adreno620FwOk);
    CHECK(image.segments == 3);
    CHECK(image.entry == ZapEntry);
    CHECK(image.segment[1].vaddr == ZapHashVaddr);
    CHECK(image.segment[2].vaddr == ZapCodeVaddr);
    CHECK(image.loaded_bytes == 0x68cu);
    CHECK(image.segment[1].filesz == 0x1a38u);
    CHECK(image.segment[1].memsz == 0x2000u); // filesz > memsz is legal for a hash segment
    CHECK(image.segment[2].loadable == 1);
    CHECK(image.segment[0].crc32 == Adreno620Crc32(parts.b00.data(), parts.b00.size()));
    CHECK(image.segment[2].crc32 == Adreno620Crc32(parts.b02.data(), parts.b02.size()));
    CHECK(image.image_crc32 == Adreno620Crc32(parts.b02.data(), parts.b02.size()));

    Adreno620MdtImage combined;
    CHECK(Adreno620MdtParse(parts.elf.data(), parts.elf.size(), nullptr, nullptr,
        0, &combined) == Adreno620FwOk);
    CHECK(combined.segments == 3 && combined.entry == 0x5000u &&
        combined.image_crc32 == image.image_crc32);

    // A changed payload changes the loaded-image CRC and fails a descriptor check.
    parts.b02[7] ^= 0x40;
    Adreno620MdtImage changed;
    CHECK(parseMdt(parts, changed) == Adreno620FwOk);
    CHECK(changed.image_crc32 != image.image_crc32);
    CHECK(Adreno620FirmwareCheck(parts.b02.data(), parts.b02.size(),
        parts.b02.size(), image.segment[2].crc32) == 0);

    // Structural refusals.
    MdtParts bad = makeMdt();
    bad.mdt[1] = 'X';
    Adreno620MdtImage scratch;
    CHECK(parseMdt(bad, scratch) == Adreno620FwBadMagic);
    bad = makeMdt();
    put16(bad.mdt, 0x12, 0x28);
    CHECK(parseMdt(bad, scratch) == Adreno620FwBadMachine);
    bad = makeMdt();
    put16(bad.mdt, 0x2c, 9);
    CHECK(parseMdt(bad, scratch) == Adreno620FwNoSpace);
    bad = makeMdt();
    bad.mdt.resize(0x50);
    CHECK(parseMdt(bad, scratch) == Adreno620FwTruncated);
    bad = makeMdt();
    bad.b02.resize(0x100);
    CHECK(parseMdt(bad, scratch) == Adreno620FwTruncated);
    bad = makeMdt();
    put32(bad.mdt, 0x18, 0x4000); // entry outside every loadable segment
    CHECK(parseMdt(bad, scratch) == Adreno620FwBadRange);
    bad = makeMdt();
    put32(bad.mdt, 0x74 + 20, 0x100); // PT_LOAD memsz < filesz
    CHECK(parseMdt(bad, scratch) == Adreno620FwBadRange);
    const uint8_t *none[3] = {nullptr, nullptr, nullptr};
    const size_t zeros[3] = {0, 0, 0};
    CHECK(Adreno620MdtParse(parts.mdt.data(), parts.mdt.size(), none, zeros, 3,
        &scratch) == Adreno620FwTruncated);
}

// ---------------------------------------------------------------------------
// SQE container
// ---------------------------------------------------------------------------
static std::vector<uint8_t> makeSqe(uint32_t version)
{
    std::vector<uint8_t> image(4 + 16, 0);
    put32(image, 0, 0); // 4-byte preamble skipped by the kernel loader
    put32(image, 4, 0x016ee000u | (version & 0xfffu));
    put32(image, 8, 0x01001ec1u);
    put32(image, 12, 0x88020001u);
    put32(image, 16, 0x11223344u);
    return image;
}

static void testSqe()
{
    std::vector<uint8_t> image = makeSqe(0x188);
    Adreno620SqeImage sqe;
    CHECK(Adreno620SqeParse(image.data(), image.size(), 0x190, &sqe) == Adreno620FwOk);
    CHECK(sqe.header == 0 && sqe.payload_bytes == 16 && sqe.version == 0x188u);
    CHECK(sqe.security_ok == 0);
    CHECK(sqe.crc32 == Adreno620Crc32(image.data() + 4, 16));
    CHECK(Adreno620SqeParse(image.data(), image.size(), 0x100, &sqe) == Adreno620FwOk);
    CHECK(sqe.security_ok == 1);
    std::vector<uint8_t> newer = makeSqe(0x190);
    CHECK(Adreno620SqeParse(newer.data(), newer.size(), 0x190, &sqe) == Adreno620FwOk);
    CHECK(sqe.security_ok == 1);
    CHECK(Adreno620SqeParse(image.data(), 8, 0, &sqe) == Adreno620FwTruncated);
    CHECK(Adreno620SqeParse(image.data(), 13, 0, &sqe) == Adreno620FwInvalid);
    CHECK(Adreno620SqeParse(nullptr, 24, 0, &sqe) == Adreno620FwInvalid);
}

// ---------------------------------------------------------------------------
// GMU firmware container
// ---------------------------------------------------------------------------
static size_t appendGmuBlock(std::vector<uint8_t> &image, uint32_t address,
    uint32_t size, uint32_t type, uint32_t value, uint8_t fill)
{
    const size_t at = image.size();
    image.resize(at + 16 + size);
    put32(image, at + 0, address);
    put32(image, at + 4, size);
    put32(image, at + 8, type);
    put32(image, at + 12, value);
    for (size_t i = 0; i < size; ++i) image[at + 16 + i] = fill;
    return at;
}

static Adreno620GmuRegions gmuRegions()
{
    const GmuLayout &layout = gmuLayout();
    return Adreno620GmuRegions{
        layout.itcm_base, layout.itcm_bytes, layout.dtcm_base, layout.dtcm_bytes,
        layout.icache_iova, layout.icache_bytes, layout.dummy_iova,
        layout.dummy_bytes,
    };
}

static std::vector<uint8_t> makeGmu()
{
    std::vector<uint8_t> image;
    appendGmuBlock(image, 0, 0, 1, 0x200, 0);              // metadata
    appendGmuBlock(image, 0x60000000u, 0, 7, 0x1000, 0);   // dummy metadata
    appendGmuBlock(image, 0x00000000u, 0xc0, 0xdeadbeefu, 0xdeadbeefu, 0x40);
    appendGmuBlock(image, 0x00004000u, 0x100, 0xdeadbeefu, 0xdeadbeefu, 0x41);
    appendGmuBlock(image, 0x10004000u, 0x80, 0xdeadbeefu, 0xdeadbeefu, 0x42);
    appendGmuBlock(image, 0x60000000u, 0x40, 0xdeadbeefu, 0xdeadbeefu, 0x43);
    return image;
}

static void testGmu()
{
    const Adreno620GmuRegions regions = gmuRegions();
    std::vector<uint8_t> image = makeGmu();
    Adreno620GmuImage gmu;
    const Adreno620FwStatus first = Adreno620GmuParse(image.data(),
        image.size(), &regions, &gmu);
    if (first != Adreno620FwOk)
        std::printf("debug gmu status=%d size=%lu\n", first,
            static_cast<unsigned long>(image.size()));
    CHECK(first == Adreno620FwOk);
    CHECK(gmu.blocks == 6 && gmu.entry == GmuEntry);
    CHECK(gmu.block[0].destination == Adreno620GmuMetadata);
    CHECK(gmu.block[1].destination == Adreno620GmuMetadata);
    CHECK(gmu.block[2].destination == Adreno620GmuItcm);
    CHECK(gmu.block[3].destination == Adreno620GmuIcache);
    CHECK(gmu.block[4].destination == Adreno620GmuDtcm);
    CHECK(gmu.block[5].destination == Adreno620GmuDummy);
    CHECK(gmu.data_bytes == 0xc0 + 0x100 + 0x80 + 0x40);
    CHECK(gmu.image_crc32 != 0);

    // Block offsets: two 16-byte metadata headers, then the ITCM data block
    // (header at 0x20, payload 0xc0), so the icache data header is at 0xf0.
    const size_t itcmHeader = 0x20;
    const size_t icacheHeader = itcmHeader + 16 + 0xc0;
    std::vector<uint8_t> noEntry = makeGmu();
    // Move the ITCM block to an unmapped address: the parser must refuse before
    // it can claim the CM3 vector table exists.
    put32(noEntry, itcmHeader, 0x50000000u);
    CHECK(Adreno620GmuParse(noEntry.data(), noEntry.size(), &regions, &gmu)
        == Adreno620FwUnmapped);

    std::vector<uint8_t> overlap = makeGmu();
    // Make the icache block start inside the ITCM block (0x0..0xc0).
    put32(overlap, icacheHeader, 0x40);
    CHECK(Adreno620GmuParse(overlap.data(), overlap.size(), &regions, &gmu)
        == Adreno620FwBadRange);

    std::vector<uint8_t> odd = makeGmu();
    put32(odd, icacheHeader + 4, 0x102);
    CHECK(Adreno620GmuParse(odd.data(), odd.size(), &regions, &gmu)
        == Adreno620FwBadRange);

    std::vector<uint8_t> truncated = makeGmu();
    truncated.resize(truncated.size() - 1);
    CHECK(Adreno620GmuParse(truncated.data(), truncated.size(), &regions, &gmu)
        == Adreno620FwTruncated);

    Adreno620GmuRegions overlapped = regions;
    overlapped.icache_base = 0x2000; // overlaps ITCM 0..0x4000
    CHECK(Adreno620GmuParse(image.data(), image.size(), &overlapped, &gmu)
        == Adreno620FwBadRange);

    // A container with only metadata has no CM3 vector table.
    std::vector<uint8_t> metadataOnly;
    appendGmuBlock(metadataOnly, 0, 0, 1, 0x200, 0);
    CHECK(Adreno620GmuParse(metadataOnly.data(), metadataOnly.size(), &regions,
        &gmu) == Adreno620FwBadRange);
}

// ---------------------------------------------------------------------------
// Probe state machine with a fake transport
// ---------------------------------------------------------------------------
enum FakeCall {
    CallClocks, CallIdentity, CallSmmu, CallFirmware, CallBoot, CallHfi,
    CallCpStart, CallCpSubmit, CallCpWait, CallDraw, CallCount
};

struct Fake {
    unsigned calls[CallCount] = {};
    unsigned order[16] = {};
    unsigned orderCount = 0;
    unsigned abortCalls = 0;
    unsigned abortStage = 0;
    int failClocks = 0, failIdentity = 0, failSmmu = 0, failFirmware = 0;
    int failBoot = 0, failHfi = 0, failCpStart = 0, failCpSubmit = 0;
    int failCpWait = 0, failDraw = 0;
    uint32_t firmwareCrc = gmu().crc32;
    uint32_t firmwareBytes = static_cast<uint32_t>(gmu().bytes);
    uint32_t fence = 7;
    uint32_t identity[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    size_t identityCount = 8;
    ProbeSmmuPlan plan{};
};

static void record(Fake &fake, FakeCall call)
{
    ++fake.calls[call];
    if (fake.orderCount < sizeof(fake.order) / sizeof(fake.order[0]))
        fake.order[fake.orderCount++] = call;
}

static int fakeClocks(void *cookie)
{
    Fake &fake = *static_cast<Fake *>(cookie);
    record(fake, CallClocks);
    return fake.failClocks ? -1 : 0;
}

static int fakeIdentity(void *cookie, uint32_t *words, size_t capacity, size_t *count)
{
    Fake &fake = *static_cast<Fake *>(cookie);
    record(fake, CallIdentity);
    if (fake.failIdentity) return -1;
    const size_t n = fake.identityCount < capacity ? fake.identityCount : capacity;
    for (size_t i = 0; i < n; ++i) words[i] = fake.identity[i];
    *count = n;
    return 0;
}

static int fakeSmmu(void *cookie, const ProbeSmmuPlan &plan)
{
    Fake &fake = *static_cast<Fake *>(cookie);
    record(fake, CallSmmu);
    fake.plan = plan;
    return fake.failSmmu ? -1 : 0;
}

static int fakeFirmware(void *cookie, uint32_t *crc32, uint32_t *bytes)
{
    Fake &fake = *static_cast<Fake *>(cookie);
    record(fake, CallFirmware);
    *crc32 = fake.firmwareCrc;
    *bytes = fake.firmwareBytes;
    return fake.failFirmware ? -1 : 0;
}

static int fakeBoot(void *cookie)
{
    Fake &fake = *static_cast<Fake *>(cookie);
    record(fake, CallBoot);
    return fake.failBoot ? -1 : 0;
}

static int fakeHfi(void *cookie)
{
    Fake &fake = *static_cast<Fake *>(cookie);
    record(fake, CallHfi);
    return fake.failHfi ? -1 : 0;
}

static int fakeCpStart(void *cookie)
{
    Fake &fake = *static_cast<Fake *>(cookie);
    record(fake, CallCpStart);
    return fake.failCpStart ? -1 : 0;
}

static int fakeCpSubmit(void *cookie, uint32_t *fence)
{
    Fake &fake = *static_cast<Fake *>(cookie);
    record(fake, CallCpSubmit);
    *fence = fake.fence;
    return fake.failCpSubmit ? -1 : 0;
}

static int fakeCpWait(void *cookie, uint32_t fence)
{
    Fake &fake = *static_cast<Fake *>(cookie);
    record(fake, CallCpWait);
    if (fence != fake.fence) return -1;
    return fake.failCpWait ? -1 : 0;
}

static int fakeDraw(void *cookie)
{
    Fake &fake = *static_cast<Fake *>(cookie);
    record(fake, CallDraw);
    return fake.failDraw ? -1 : 0;
}

static void fakeAbort(void *cookie, uint32_t stage)
{
    Fake &fake = *static_cast<Fake *>(cookie);
    ++fake.abortCalls;
    fake.abortStage = stage;
}

static ProbeOps makeOps(Fake &fake)
{
    ProbeOps ops{};
    ops.cookie = &fake;
    ops.clocks = fakeClocks;
    ops.identity = fakeIdentity;
    ops.smmu = fakeSmmu;
    ops.gmuFirmware = fakeFirmware;
    ops.gmuBoot = fakeBoot;
    ops.hfi = fakeHfi;
    ops.cpStart = fakeCpStart;
    ops.cpSubmit = fakeCpSubmit;
    ops.cpWait = fakeCpWait;
    ops.draw = fakeDraw;
    ops.abort = fakeAbort;
    return ops;
}

static void runToEnd(Probe &probe, unsigned limit)
{
    for (unsigned tick = 0; tick < limit; ++tick) {
        const int result = probe.tick();
        if (result || probe.diagnostics().finished || probe.diagnostics().failed)
            return;
    }
}

static void testProbeSuccess()
{
    Fake fake;
    Probe probe;
    ProbeOps ops = makeOps(fake);
    CHECK(probe.configure(5, &ops) == ProbeOk);
    runToEnd(probe, 16);
    CHECK(probe.diagnostics().finished);
    CHECK(!probe.diagnostics().failed);
    CHECK(probe.diagnostics().error == ProbeOk);
    CHECK(probe.diagnostics().ticks == 5);
    CHECK(probe.diagnostics().stage == ProbeDraw);
    CHECK(probe.diagnostics().step == 2);
    for (unsigned call = 0; call < CallCount; ++call) CHECK(fake.calls[call] == 1);
    const unsigned expected[10] = {
        CallClocks, CallIdentity, CallSmmu, CallFirmware, CallBoot, CallHfi,
        CallCpStart, CallCpSubmit, CallCpWait, CallDraw,
    };
    CHECK(fake.orderCount == 10);
    for (unsigned i = 0; i < 10; ++i) CHECK(fake.order[i] == expected[i]);
    CHECK(fake.abortCalls == 0);
    CHECK(probe.diagnostics().identity_count == 8);
    CHECK(probe.diagnostics().identity[7] == 8);
    CHECK(probe.diagnostics().smmu_banks == 2);
    CHECK(probe.diagnostics().smmu_sids == 6);
    CHECK(probe.diagnostics().firmware_crc == gmu().crc32);
    CHECK(probe.diagnostics().firmware_bytes == gmu().bytes);
    CHECK(probe.diagnostics().fence == fake.fence);
    CHECK(probe.smmuPlan().banks == 4);
    CHECK(probe.smmuPlan().bank[0].sid_count == 4);
    CHECK(probe.smmuPlan().bank[3].sid_count == 2);
    CHECK(probe.smmuPlan().bank[0].sids[2] == 0x400);
    CHECK(probe.smmuPlan().bank[3].sids[1] == 0x405);
}

static void testProbeEarlierStage()
{
    Fake fake;
    Probe probe;
    ProbeOps ops = makeOps(fake);
    CHECK(probe.configure(1, &ops) == ProbeOk);
    CHECK(probe.tick() == ProbeOk);
    CHECK(probe.diagnostics().finished);
    CHECK(probe.diagnostics().ticks == 1);
    CHECK(fake.calls[CallClocks] == 1 && fake.calls[CallIdentity] == 1);
    CHECK(fake.calls[CallSmmu] == 0);
}

static void testProbeFailures()
{
    {   // clocks
        Fake fake; Probe probe; ProbeOps ops = makeOps(fake);
        fake.failClocks = 1;
        CHECK(probe.configure(5, &ops) == ProbeOk);
        CHECK(probe.tick() == ProbeClockFailure);
        CHECK(probe.diagnostics().failed && !probe.diagnostics().finished);
        CHECK(probe.diagnostics().error == ProbeClockFailure);
        CHECK(fake.abortCalls == 1 && fake.abortStage == ProbeClocks);
        CHECK(fake.calls[CallIdentity] == 0);
        const unsigned before = fake.calls[CallClocks];
        CHECK(probe.tick() == ProbeClockFailure); // no retry
        CHECK(fake.calls[CallClocks] == before);
        CHECK(fake.abortCalls == 1);
    }
    {   // identity
        Fake fake; Probe probe; ProbeOps ops = makeOps(fake);
        fake.failIdentity = 1;
        CHECK(probe.configure(5, &ops) == ProbeOk);
        CHECK(probe.tick() == ProbeIdentityFailure);
        CHECK(fake.abortStage == ProbeClocks);
        CHECK(fake.calls[CallSmmu] == 0);
    }
    {   // SMMU op
        Fake fake; Probe probe; ProbeOps ops = makeOps(fake);
        fake.failSmmu = 1;
        CHECK(probe.configure(5, &ops) == ProbeOk);
        CHECK(probe.tick() == ProbeOk);
        CHECK(probe.tick() == ProbeSmmuFailure);
        CHECK(fake.abortStage == ProbeSmmu);
        CHECK(probe.diagnostics().smmu_banks == 2);
        CHECK(probe.diagnostics().smmu_sids == 6);
        CHECK(fake.calls[CallFirmware] == 0);
    }
    {   // firmware CRC
        Fake fake; Probe probe; ProbeOps ops = makeOps(fake);
        fake.firmwareCrc ^= 1;
        CHECK(probe.configure(5, &ops) == ProbeOk);
        CHECK(probe.tick() == ProbeOk);
        CHECK(probe.tick() == ProbeOk);
        CHECK(probe.tick() == ProbeGmuFirmwareFailure);
        CHECK(fake.abortStage == ProbeGmu);
        CHECK(fake.calls[CallBoot] == 0 && fake.calls[CallHfi] == 0);
    }
    {   // firmware op
        Fake fake; Probe probe; ProbeOps ops = makeOps(fake);
        fake.failFirmware = 1;
        CHECK(probe.configure(5, &ops) == ProbeOk);
        CHECK(probe.tick() == ProbeOk);
        CHECK(probe.tick() == ProbeOk);
        CHECK(probe.tick() == ProbeGmuFirmwareFailure);
    }
    {   // GMU boot
        Fake fake; Probe probe; ProbeOps ops = makeOps(fake);
        fake.failBoot = 1;
        CHECK(probe.configure(5, &ops) == ProbeOk);
        CHECK(probe.tick() == ProbeOk);
        CHECK(probe.tick() == ProbeOk);
        CHECK(probe.tick() == ProbeGmuBootFailure);
        CHECK(fake.calls[CallHfi] == 0);
    }
    {   // HFI
        Fake fake; Probe probe; ProbeOps ops = makeOps(fake);
        fake.failHfi = 1;
        CHECK(probe.configure(5, &ops) == ProbeOk);
        CHECK(probe.tick() == ProbeOk);
        CHECK(probe.tick() == ProbeOk);
        CHECK(probe.tick() == ProbeHfiFailure);
    }
    {   // CP start
        Fake fake; Probe probe; ProbeOps ops = makeOps(fake);
        fake.failCpStart = 1;
        CHECK(probe.configure(5, &ops) == ProbeOk);
        CHECK(probe.tick() == ProbeOk);
        CHECK(probe.tick() == ProbeOk);
        CHECK(probe.tick() == ProbeOk);
        CHECK(probe.tick() == ProbeCpFailure);
        CHECK(fake.calls[CallCpSubmit] == 0 && fake.calls[CallCpWait] == 0);
    }
    {   // CP submit
        Fake fake; Probe probe; ProbeOps ops = makeOps(fake);
        fake.failCpSubmit = 1;
        CHECK(probe.configure(5, &ops) == ProbeOk);
        CHECK(probe.tick() == ProbeOk);
        CHECK(probe.tick() == ProbeOk);
        CHECK(probe.tick() == ProbeOk);
        CHECK(probe.tick() == ProbeCpFailure);
        CHECK(fake.calls[CallCpWait] == 0);
    }
    {   // CP fence wait
        Fake fake; Probe probe; ProbeOps ops = makeOps(fake);
        fake.failCpWait = 1;
        CHECK(probe.configure(5, &ops) == ProbeOk);
        CHECK(probe.tick() == ProbeOk);
        CHECK(probe.tick() == ProbeOk);
        CHECK(probe.tick() == ProbeOk);
        CHECK(probe.tick() == ProbeCpFenceFailure);
    }
    {   // draw
        Fake fake; Probe probe; ProbeOps ops = makeOps(fake);
        fake.failDraw = 1;
        CHECK(probe.configure(5, &ops) == ProbeOk);
        CHECK(probe.tick() == ProbeOk);
        CHECK(probe.tick() == ProbeOk);
        CHECK(probe.tick() == ProbeOk);
        CHECK(probe.tick() == ProbeOk);
        CHECK(probe.tick() == ProbeDrawUnavailable);
    }
    {   // missing SMMU callback
        Fake fake; Probe probe; ProbeOps ops = makeOps(fake);
        ops.smmu = nullptr;
        CHECK(probe.configure(2, &ops) == ProbeOk);
        CHECK(probe.tick() == ProbeOk);
        CHECK(probe.tick() == ProbeSmmuFailure);
        CHECK(probe.diagnostics().stage == ProbeSmmu);
        CHECK(fake.abortStage == ProbeSmmu);
    }
    {   // bad request and double configuration
        Fake fake; Probe probe; ProbeOps ops = makeOps(fake);
        CHECK(probe.tick() == ProbeBadRequest);
        CHECK(probe.configure(0, &ops) == ProbeBadRequest);
        CHECK(probe.configure(6, &ops) == ProbeBadRequest);
        CHECK(probe.configure(1, &ops) == ProbeOk);
        CHECK(probe.configure(1, &ops) == ProbeBadRequest);
    }
}

// ---------------------------------------------------------------------------
// Real vendor blobs, only when ADRENO620_VENDOR_DIR is set
// ---------------------------------------------------------------------------
static void testVendor(const char *directory)
{
    const std::string dir(directory);
    std::vector<uint8_t> mdt = readFile(dir + "/a620_zap.mdt");
    std::vector<uint8_t> b00 = readFile(dir + "/a620_zap.b00");
    std::vector<uint8_t> b01 = readFile(dir + "/a620_zap.b01");
    std::vector<uint8_t> b02 = readFile(dir + "/a620_zap.b02");
    std::vector<uint8_t> elf = readFile(dir + "/a620_zap.elf");
    std::vector<uint8_t> a630 = readFile(dir + "/a630_sqe.fw");
    std::vector<uint8_t> a650 = readFile(dir + "/a650_sqe.fw");
    std::vector<uint8_t> gmuImage = readFile(dir + "/a650_gmu.bin");

    CHECK(Adreno620FirmwareCheck(mdt.data(), mdt.size(), zapMdt().bytes,
        zapMdt().crc32));
    CHECK(Adreno620FirmwareCheck(b00.data(), b00.size(), zapSegment(0).bytes,
        zapSegment(0).crc32));
    CHECK(Adreno620FirmwareCheck(b01.data(), b01.size(), zapSegment(1).bytes,
        zapSegment(1).crc32));
    CHECK(Adreno620FirmwareCheck(b02.data(), b02.size(), zapSegment(2).bytes,
        zapSegment(2).crc32));
    CHECK(Adreno620FirmwareCheck(elf.data(), elf.size(), zapElf().bytes,
        zapElf().crc32));
    CHECK(Adreno620FirmwareCheck(a630.data(), a630.size(), sqe().bytes,
        sqe().crc32));
    CHECK(sqeAlternate() && Adreno620FirmwareCheck(a650.data(), a650.size(),
        sqeAlternate()->bytes, sqeAlternate()->crc32));
    CHECK(Adreno620FirmwareCheck(gmuImage.data(), gmuImage.size(), gmu().bytes,
        gmu().crc32));

    const uint8_t *segments[3] = {b00.data(), b01.data(), b02.data()};
    const size_t sizes[3] = {b00.size(), b01.size(), b02.size()};
    Adreno620MdtImage zap;
    CHECK(Adreno620MdtParse(mdt.data(), mdt.size(), segments, sizes, 3, &zap)
        == Adreno620FwOk);
    Adreno620MdtImage combined;
    CHECK(Adreno620MdtParse(elf.data(), elf.size(), nullptr, nullptr, 0,
        &combined) == Adreno620FwOk);
    CHECK(zap.image_crc32 == combined.image_crc32);
    std::printf("vendor a620_zap.mdt: %lu bytes crc=%08x entry=%08x segments=%lu "
                "loaded=%llu\n", static_cast<unsigned long>(mdt.size()),
        zapMdt().crc32, zap.entry, static_cast<unsigned long>(zap.segments),
        static_cast<unsigned long long>(zap.loaded_bytes));
    for (size_t i = 0; i < zap.segments; ++i) {
        const Adreno620MdtSegment &segment = zap.segment[i];
        std::printf("  seg%lu type=%u vaddr=%08llx filesz=%llu memsz=%llu "
                    "crc=%08x\n", static_cast<unsigned long>(i), segment.type,
            static_cast<unsigned long long>(segment.vaddr),
            static_cast<unsigned long long>(segment.filesz),
            static_cast<unsigned long long>(segment.memsz), segment.crc32);
    }

    Adreno620SqeImage sqeImage;
    CHECK(Adreno620SqeParse(a630.data(), a630.size(), 0x190, &sqeImage)
        == Adreno620FwOk);
    CHECK(sqeImage.version == 0x197u && sqeImage.security_ok == 1);
    std::printf("vendor a630_sqe.fw: %lu bytes payload=%lu version=0x%x "
                "security=%u crc=%08x\n", static_cast<unsigned long>(a630.size()),
        static_cast<unsigned long>(sqeImage.payload_bytes), sqeImage.version,
        sqeImage.security_ok, sqeImage.crc32);
    CHECK(Adreno620SqeParse(a650.data(), a650.size(), 0x095, &sqeImage)
        == Adreno620FwOk);
    CHECK(sqeImage.version == 0x093u && sqeImage.security_ok == 0);
    std::printf("vendor a650_sqe.fw: %lu bytes payload=%lu version=0x%x "
                "security=%u crc=%08x\n", static_cast<unsigned long>(a650.size()),
        static_cast<unsigned long>(sqeImage.payload_bytes), sqeImage.version,
        sqeImage.security_ok, sqeImage.crc32);

    const Adreno620GmuRegions regions = gmuRegions();
    Adreno620GmuImage gmu;
    CHECK(Adreno620GmuParse(gmuImage.data(), gmuImage.size(), &regions, &gmu)
        == Adreno620FwOk);
    CHECK(gmu.blocks == 16 && gmu.data_bytes == 41292);
    std::printf("vendor a650_gmu.bin: %lu bytes blocks=%lu data=%llu "
                "image_crc=%08x entry=%u\n",
        static_cast<unsigned long>(gmuImage.size()),
        static_cast<unsigned long>(gmu.blocks),
        static_cast<unsigned long long>(gmu.data_bytes), gmu.image_crc32,
        gmu.entry);
    for (size_t i = 0; i < gmu.blocks; ++i) {
        const Adreno620GmuBlock &block = gmu.block[i];
        std::printf("  blk%02lu addr=%08x size=%06x type=%08x dest=%u crc=%08x\n",
            static_cast<unsigned long>(i), block.address, block.bytes,
            block.type, block.destination, block.crc32);
    }
}

int main()
{
    testCrc();
    testDt();
    testBoard();
    testMdt();
    testSqe();
    testGmu();
    testProbeSuccess();
    testProbeEarlierStage();
    testProbeFailures();
    const char *vendor = std::getenv("ADRENO620_VENDOR_DIR");
    if (vendor && *vendor) {
        testVendor(vendor);
        std::printf("vendor validation: PASS\n");
    } else {
        std::printf("vendor validation skipped (ADRENO620_VENDOR_DIR unset)\n");
    }
    std::printf("adreno620 host tests: PASS\n");
    return 0;
}
