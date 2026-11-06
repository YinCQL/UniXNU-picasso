// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests for NativeWlan. No hardware: synthetic MBN/BDF containers, the board math, the WLFW handshake and the
// staged probe with fake callbacks. If argv[1] names a real extracted firmware base (for example
// /tmp/wlan-probe/real/ipa_fws), the matching .mdt/.b00.../.elf files are read read-only and cross-checked.
#include "WlanBoard.h"
#include "WlanFirmware.h"
#include "WlanSmem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static void put16(uint8_t *p, size_t at, uint16_t v) { p[at] = (uint8_t)v; p[at + 1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, size_t at, uint32_t v) {
    p[at] = (uint8_t)v; p[at + 1] = (uint8_t)(v >> 8); p[at + 2] = (uint8_t)(v >> 16); p[at + 3] = (uint8_t)(v >> 24);
}
// ---------------------------------------------------------------------------------------------------------------
// Synthetic ELF32/MBN: three program headers, header 0 is the hash segment, payloads at 0x1000 and 0x2000.
// ---------------------------------------------------------------------------------------------------------------
#define T_PHOFF 52u
#define T_PHNUM 3u
#define T_HASH_OFF (T_PHOFF + T_PHNUM * 32u)
#define T_HASH_BYTES (2u * WLAN_MBN_HASH_ENTRY)
#define T_PAY1_OFF 0x1000u
#define T_PAY1_LEN 64u
#define T_PAY2_OFF 0x2000u
#define T_PAY2_LEN 32u
#define T_FILE_LEN (T_PAY2_OFF + T_PAY2_LEN)

static void put_phdr(uint8_t *p, size_t at, uint32_t type, uint32_t off, uint32_t vaddr, uint32_t filesz,
                     uint32_t memsz) {
    put32(p, at + 0, type); put32(p, at + 4, off); put32(p, at + 8, vaddr); put32(p, at + 12, vaddr);
    put32(p, at + 16, filesz); put32(p, at + 20, memsz); put32(p, at + 24, 5); put32(p, at + 28, 4);
}

static uint8_t *make_mbn(void) {
    uint8_t *b = calloc(1, T_FILE_LEN);
    CHECK(b);
    b[0] = 0x7f; b[1] = 'E'; b[2] = 'L'; b[3] = 'F';
    b[4] = 1; b[5] = 1; b[6] = 1;
    put16(b, 16, 2);          // ET_EXEC
    put16(b, 18, 40);         // EM_ARM
    put32(b, 20, 1);          // e_version
    put32(b, 28, T_PHOFF);
    put32(b, 32, 0);          // e_shoff
    put16(b, 40, 52);         // e_ehsize
    put16(b, 42, 32);         // e_phentsize
    put16(b, 44, T_PHNUM);
    put_phdr(b, T_PHOFF + 0 * 32, 1, 0, 0, 0, T_HASH_BYTES);        // hash segment
    put_phdr(b, T_PHOFF + 1 * 32, 1, T_PAY1_OFF, 0x1000, T_PAY1_LEN, T_PAY1_LEN);
    put_phdr(b, T_PHOFF + 2 * 32, 1, T_PAY2_OFF, 0x2000, T_PAY2_LEN, T_PAY2_LEN);
    for (unsigned i = 0; i < T_PAY1_LEN; ++i) b[T_PAY1_OFF + i] = (uint8_t)(0x40 + i);
    for (unsigned i = 0; i < T_PAY2_LEN; ++i) b[T_PAY2_OFF + i] = (uint8_t)(0x90 + i);
    uint8_t d1[WLAN_MBN_HASH_LEN], d2[WLAN_MBN_HASH_LEN];
    wlan_sha1(b + T_PAY1_OFF, T_PAY1_LEN, d1);
    wlan_sha1(b + T_PAY2_OFF, T_PAY2_LEN, d2);
    put32(b, T_HASH_OFF, T_PAY1_LEN);
    memcpy(b + T_HASH_OFF + 4, d1, sizeof d1);
    put32(b, T_HASH_OFF + WLAN_MBN_HASH_ENTRY, T_PAY2_LEN);
    memcpy(b + T_HASH_OFF + WLAN_MBN_HASH_ENTRY + 4, d2, sizeof d2);
    return b;
}

// ---------------------------------------------------------------------------------------------------------------
// Real split-MDT cross-check (read-only; skipped when the files are not present).
// ---------------------------------------------------------------------------------------------------------------
static uint8_t *load_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return NULL; }
    const long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET)) { fclose(f); return NULL; }
    uint8_t *d = malloc((size_t)n ? (size_t)n : 1);
    if (!d) { fclose(f); return NULL; }
    if (n && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); fclose(f); return NULL; }
    fclose(f);
    *size = (size_t)n;
    return d;
}

static void real_mdt_test(const char *base) {
    if (!base || !*base) {
        printf("SKIP real MBN: pass a firmware base path as argv[1] (e.g. /tmp/wlan-probe/real/ipa_fws)\n");
        return;
    }
    char path[512];
    size_t mdt_size = 0, elf_size = 0;
    snprintf(path, sizeof path, "%s.mdt", base);
    uint8_t *mdt = load_file(path, &mdt_size);
    snprintf(path, sizeof path, "%s.elf", base);
    uint8_t *elf = load_file(path, &elf_size);
    if (!mdt || !elf) {
        printf("SKIP real MBN: %s.mdt/.elf not present\n", base);
        free(mdt); free(elf);
        return;
    }
    struct wlan_mbn hm, he, hs;
    CHECK(wlan_mbn_parse_header(&hm, mdt, mdt_size) == 0);
    CHECK(wlan_mbn_parse(&he, elf, elf_size) == 0);
    CHECK(hm.segments == he.segments);
    CHECK(hm.machine == he.machine);
    CHECK(hm.hash_index == WLAN_MBN_NO_HASH);   // the IPA image is unsigned
    CHECK(he.hash_index == WLAN_MBN_NO_HASH);
    CHECK(wlan_mbn_verify_hashes(&he, elf, elf_size) == 0);

    const uint8_t *parts[WLAN_MBN_MAX_SEGMENTS] = {0};
    size_t part_sizes[WLAN_MBN_MAX_SEGMENTS] = {0};
    for (unsigned i = 0; i < hm.segments; ++i) {
        snprintf(path, sizeof path, "%s.b%02u", base, i);
        parts[i] = load_file(path, &part_sizes[i]);
        CHECK(parts[i] != NULL);
    }
    CHECK(wlan_mbn_parse_split(&hs, mdt, mdt_size, parts, part_sizes, hm.segments) == 0);
    for (unsigned i = 0; i < hm.segments; ++i) {
        CHECK(part_sizes[i] >= he.seg[i].filesz);
        CHECK(memcmp(elf + he.seg[i].offset, parts[i], he.seg[i].filesz) == 0);
    }
    printf("PASS real MBN %s: segments=%u machine=%u payload=%llu bytes; split .b00.. map onto the .elf image\n",
           base, hm.segments, (unsigned)hm.machine, (unsigned long long)hm.payload_size);
    for (unsigned i = 0; i < hm.segments; ++i) free((void *)parts[i]);
    free(mdt); free(elf);
}

// ---------------------------------------------------------------------------------------------------------------
// Fake WLFW transport and fake staged probe.
// ---------------------------------------------------------------------------------------------------------------
struct fake_hs {
    unsigned requests[8], request_count;
    unsigned indications[4], indication_count;
    unsigned fail_request, fail_load, fail_wait;
    int msa_ok;
};
static int fake_load(void *ctx, const struct wlan_mbn *image) {
    struct fake_hs *f = ctx;
    CHECK(image != NULL);
    return f->fail_load;
}
static int fake_request(void *ctx, unsigned request, uint64_t a0, uint64_t a1) {
    struct fake_hs *f = ctx;
    if (f->request_count < 8) f->requests[f->request_count] = request;
    ++f->request_count;
    if (f->fail_request == request) return -5;
    if (request == WLAN_WLFW_MSA_MEM_INFO && (a0 != WLAN_MSA_BASE || a1 != WLAN_MSA_SIZE)) return -22;
    return 0;
}
static int fake_wait(void *ctx, unsigned indication, unsigned timeout_ms) {
    struct fake_hs *f = ctx;
    CHECK(timeout_ms <= 100);
    if (f->indication_count < 4) f->indications[f->indication_count] = indication;
    ++f->indication_count;
    if (f->fail_wait == indication) return WLAN_ERR_TIMEOUT;
    return 0;
}

static void handshake_tests(const struct wlan_mbn *image) {
    struct wlan_handshake_ops ops = {0};
    struct wlan_handshake_state st;
    struct fake_hs f;

    memset(&f, 0, sizeof f);
    ops.ctx = &f; ops.load_image = fake_load; ops.request = fake_request; ops.wait_indication = fake_wait;
    CHECK(wlan_handshake_run(&ops, image, WLAN_MSA_BASE, WLAN_MSA_SIZE, 50, &st) == 0);
    CHECK(st.step == WLAN_HS_DONE && st.error == 0 && st.requests == 6);
    CHECK(f.request_count == 6);
    CHECK(f.requests[0] == WLAN_WLFW_CAPABILITY && f.requests[1] == WLAN_WLFW_CONFIG);
    CHECK(f.requests[2] == WLAN_WLFW_IND_REGISTER && f.requests[3] == WLAN_WLFW_INI);
    CHECK(f.requests[4] == WLAN_WLFW_MSA_MEM_INFO && f.requests[5] == WLAN_WLFW_MODE);
    CHECK(f.indication_count == 2 && f.indications[0] == WLAN_WLFW_IND_FW_READY);
    CHECK(f.indications[1] == WLAN_WLFW_IND_MSA_READY);

    memset(&f, 0, sizeof f); f.fail_load = -45;
    CHECK(wlan_handshake_run(&ops, image, WLAN_MSA_BASE, WLAN_MSA_SIZE, 50, &st) == -45);
    CHECK(st.step == WLAN_HS_LOAD && st.error == -45);

    memset(&f, 0, sizeof f); f.fail_request = WLAN_WLFW_CAPABILITY;
    CHECK(wlan_handshake_run(&ops, image, WLAN_MSA_BASE, WLAN_MSA_SIZE, 50, &st) == -5);
    CHECK(st.step == WLAN_HS_CAPABILITY && st.error == -5);

    memset(&f, 0, sizeof f); f.fail_wait = WLAN_WLFW_IND_FW_READY;
    CHECK(wlan_handshake_run(&ops, image, WLAN_MSA_BASE, WLAN_MSA_SIZE, 50, &st) == WLAN_ERR_TIMEOUT);
    CHECK(st.step == WLAN_HS_FW_READY && st.error == WLAN_ERR_TIMEOUT);

    // An unaligned or empty MSA region is refused before the QMI request.
    memset(&f, 0, sizeof f);
    CHECK(wlan_handshake_run(&ops, image, WLAN_MSA_BASE + 1, WLAN_MSA_SIZE, 50, &st) == WLAN_ERR_INVAL);
    CHECK(st.step == WLAN_HS_MSA_INFO && st.error == WLAN_ERR_INVAL);
    CHECK(f.request_count == 4);   // cap/config/ind/ini were sent; MSA info was not

    memset(&f, 0, sizeof f); f.fail_wait = WLAN_WLFW_IND_MSA_READY;
    CHECK(wlan_handshake_run(&ops, image, WLAN_MSA_BASE, WLAN_MSA_SIZE, 50, &st) == WLAN_ERR_TIMEOUT);
    CHECK(st.step == WLAN_HS_MSA_READY);

    memset(&f, 0, sizeof f); f.fail_request = WLAN_WLFW_MODE;
    CHECK(wlan_handshake_run(&ops, image, WLAN_MSA_BASE, WLAN_MSA_SIZE, 50, &st) == -5);
    CHECK(st.step == WLAN_HS_MODE_ON);
}

struct fake_probe { unsigned calls[6]; int fail_stage, fail_code; };
static int fake_stage1(void *ctx) { ((struct fake_probe *)ctx)->calls[1]++; return 0; }
static int fake_stage2(void *ctx) { ((struct fake_probe *)ctx)->calls[2]++; return 0; }
static int fake_stage3(void *ctx) {
    struct fake_probe *f = ctx; ++f->calls[3];
    return f->fail_stage == 3 ? f->fail_code : 0;
}
static int fake_stage4(void *ctx) { ((struct fake_probe *)ctx)->calls[4]++; return 0; }
static int fake_stage5(void *ctx) {
    struct fake_probe *f = ctx; ++f->calls[5];
    return f->fail_stage == 5 ? f->fail_code : 0;
}

static void probe_tests(void) {
    struct wlan_probe_ops ops = {0};
    struct wlan_probe_state st;
    struct fake_probe f;
    ops.stage1 = fake_stage1; ops.stage2 = fake_stage2; ops.stage3 = fake_stage3;
    ops.stage4 = fake_stage4; ops.stage5 = fake_stage5;

    memset(&f, 0, sizeof f); ops.ctx = &f;
    CHECK(wlan_probe_run(&ops, 5, &st) == 0);
    CHECK(st.stage == 5 && st.step == 5 && st.error == 0);
    for (unsigned i = 1; i <= 5; ++i) CHECK(f.calls[i] == 1);

    memset(&f, 0, sizeof f);
    CHECK(wlan_probe_run(&ops, 2, &st) == 0);
    CHECK(st.stage == 2 && f.calls[1] == 1 && f.calls[2] == 1 && f.calls[3] == 0 && f.calls[5] == 0);

    memset(&f, 0, sizeof f); f.fail_stage = 3; f.fail_code = -5;
    CHECK(wlan_probe_run(&ops, 5, &st) == -5);
    CHECK(st.stage == 3 && st.step == 3 && st.error == -5);
    CHECK(f.calls[4] == 0 && f.calls[5] == 0);

    // On the real board stage 5 is the honest "no Apple 802.11 stack" stop.
    memset(&f, 0, sizeof f); f.fail_stage = 5; f.fail_code = WLAN_ERR_NOTSUP;
    CHECK(wlan_probe_run(&ops, 5, &st) == WLAN_ERR_NOTSUP);
    CHECK(st.stage == 5 && st.error == WLAN_ERR_NOTSUP);

    CHECK(wlan_probe_run(&ops, 0, &st) == WLAN_ERR_INVAL);
    CHECK(wlan_probe_run(&ops, 6, &st) == WLAN_ERR_INVAL);
}

// ---------------------------------------------------------------------------------------------------------------

static uint8_t smem_fixture[WLAN_SMEM_SIZE];
static unsigned smem_reads,smem_mutate;
static int smem_read_test(void *ctx,uint32_t offset,void *out,uint32_t size) {
    (void)ctx;CHECK(offset<=sizeof smem_fixture && size<=sizeof smem_fixture-offset);
    ++smem_reads;memcpy(out,smem_fixture+offset,size);
    if (smem_mutate && size==20 && offset==0x1000) ((uint8_t *)out)[12]^=8;
    return 0;
}
static void smem_prepare(void) {
    memset(smem_fixture,0,sizeof smem_fixture);smem_reads=smem_mutate=0;
    put32(smem_fixture,0x5c,12U<<16);
    unsigned toc=WLAN_SMEM_SIZE-4096;uint8_t *t=smem_fixture+toc;
    memcpy(t,"$TOC",4);put32(t,4,1);put32(t,8,1);
    put32(t,32,0x1000);put32(t,36,0x1000);t[46]=1;put32(t,48,64);
    uint8_t *h=smem_fixture+0x1000;memcpy(h,"$PRT",4);h[6]=1;
    put32(h,8,0x1000);put32(h,12,32+16+344);put32(h,16,0x1000);
    h[32]=0xa5;h[33]=0xa5;h[34]=435&255;h[35]=435>>8;put32(h,36,344);h[40]=4;
    uint8_t *v=h+48;memcpy(v,"$SMP",4);v[4]=1;v[8]=1;v[12]=16;v[14]=1;
    memcpy(v+20,"wlan",5);put32(v,36,3);
}
static void smem_tests(void) {
    struct wlan_smem_io io={NULL,smem_read_test};struct wlan_smem_partition p;
    struct wlan_smem_item item;uint32_t value;
    smem_prepare();CHECK(!wlan_smem_partition(&io,0,1,&p));CHECK(p.offset==0x1000 && p.size==0x1000);
    CHECK(!wlan_smem_find(&io,&p,435,&item));CHECK(item.offset==0x1030 && item.size==340);
    CHECK(!wlan_smp2p_wlan(&io,&item,1,0,&value) && value==3);
    CHECK(wlan_smem_find(&io,&p,428,&item)==-2);
    CHECK(wlan_smp2p_wlan(&io,&item,0,1,&value)==-22);
    for (unsigned n=0;n<10;++n) {
        smem_prepare();uint8_t *t=smem_fixture+WLAN_SMEM_SIZE-4096,*h=smem_fixture+0x1000;
        switch(n) {
        case 0:t[0]=0;break;case 1:put32(t,8,65);break;case 2:put32(t,36,0xffffffff);break;
        case 3:put32(t,48,3);break;case 4:h[6]=2;break;case 5:put32(h,12,16);break;
        case 6:put32(h,16,4097);break;case 7:put32(h,12,4096);put32(h,16,4088);break;
        case 8:put32(t,8,2);memcpy(t+80,t+32,48);break;case 9:put32(smem_fixture,0x5c,11U<<16);break;
        }
        CHECK(wlan_smem_partition(&io,0,1,&p)==-22);
    }
    for (unsigned n=0;n<7;++n) {
        smem_prepare();CHECK(!wlan_smem_partition(&io,0,1,&p));uint8_t *h=smem_fixture+0x1000;
        switch(n) {
        case 0:h[32]=0;break;case 1:put32(h,36,0xffffffff);break;case 2:h[40]=255;h[41]=255;break;
        case 3:h[42]=255;h[43]=255;break;case 4:put32(h,36,343);break;
        case 5:smem_mutate=1;break;case 6:p.cacheline=0;break;
        }
        CHECK(wlan_smem_find(&io,&p,435,&item)==(n==5?-11:-22));CHECK(smem_reads<270);
    }
    // Cached items live below an aligned header, not after it.
    smem_prepare();uint8_t *h=smem_fixture+0x1000;put32(h,12,32);put32(h,16,4096-64-384);
    memmove(h+4096-64-384,h+48,340);memmove(h+4096-64,h+32,16);put32(h,4096-64+4,384);h[4096-64+8]=44;
    CHECK(!wlan_smem_partition(&io,0,1,&p));CHECK(!wlan_smem_find(&io,&p,435,&item));
    CHECK(item.offset==0x1000+4096-64-384 && item.size==340);
    CHECK(!wlan_smp2p_wlan(&io,&item,1,0,&value));
    struct wlan_smem_item outside={WLAN_SMEM_SIZE-8,340};
    unsigned before=smem_reads;
    CHECK(wlan_smp2p_wlan(&io,&outside,1,0,&value)==-22 && before==smem_reads);
    for(unsigned n=0;n<4;++n) {
        smem_prepare();CHECK(!wlan_smem_partition(&io,0,1,&p));CHECK(!wlan_smem_find(&io,&p,435,&item));
        uint8_t *v=smem_fixture+item.offset;
        if(n==0)v[0]=0;if(n==1)v[4]=2;if(n==2)v[14]=17;if(n==3)item.size=339;
        CHECK(wlan_smp2p_wlan(&io,&item,1,0,&value)==-22);
    }
    puts("PASS: bounded SMEM/SMP2P partition, cached/uncached items, identity, truncation and concurrent allocation");
}

int main(int argc, char **argv) {
    uint8_t sha[WLAN_MBN_HASH_LEN];
    static const uint8_t abc[] = {'a', 'b', 'c'};
    static const uint8_t abc_sha[WLAN_MBN_HASH_LEN] = {
        0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
        0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d,
    };
    static const uint8_t empty_sha[WLAN_MBN_HASH_LEN] = {
        0xda, 0x39, 0xa3, 0xee, 0x5e, 0x6b, 0x4b, 0x0d, 0x32, 0x55,
        0xbf, 0xef, 0x95, 0x60, 0x18, 0x90, 0xaf, 0xd8, 0x07, 0x09,
    };
    wlan_sha1(abc, sizeof abc, sha);
    CHECK(memcmp(sha, abc_sha, sizeof sha) == 0);
    wlan_sha1(NULL, 0, sha);
    CHECK(memcmp(sha, empty_sha, sizeof sha) == 0);

    // Inventory: names, hashes and a few board-defining entries.
    CHECK(wlan_firmware_count >= 15);
    for (unsigned i = 0; i < wlan_firmware_count; ++i) {
        const struct wlan_fw_inventory *e = &wlan_firmware_inventory[i];
        CHECK(e->name && e->role && e->source && e->variant);
        CHECK(e->kind >= WLAN_FW_OPAQUE && e->kind <= WLAN_FW_KO);
        if (e->sha256) CHECK(strlen(e->sha256) == 64);
        if (e->present) { CHECK(e->size > 0); CHECK(e->sha256 != NULL); }
    }
    CHECK(wlan_firmware_inventory[2].kind == WLAN_FW_OPAQUE);   // athwlan.bin
    CHECK(strcmp(wlan_firmware_inventory[8].name, "bdwlan.%03x.b%02x") == 0);

    // Regulators: ranges, targets and clamping.
    CHECK(WLAN_REGULATOR_COUNT == 5);
    const struct wlan_regulator *r = wlan_regulator_find("vdd-3.3-ch0");
    CHECK(r && r->node && strcmp(r->node, "pm8150a_l10") == 0);
    CHECK(wlan_regulator_target_uv(r) == 3312000);
    CHECK(wlan_regulator_validate(r, 3000000) == 0);
    CHECK(wlan_regulator_validate(r, 3312000) == 0);
    CHECK(wlan_regulator_validate(r, 2999999) == -22);
    CHECK(wlan_regulator_validate(r, 3312001) == -22);
    uint32_t v = 0;
    CHECK(wlan_regulator_clamp(r, 3400000, &v) == 0 && v == 3312000);
    CHECK(wlan_regulator_clamp(r, 1000000, &v) == 0 && v == 3000000);
    const struct wlan_regulator *cx = wlan_regulator_find("vdd-cx-mx");
    CHECK(cx && wlan_regulator_target_uv(cx) == 0);   // no override: leave the bootloader value
    CHECK(wlan_regulator_validate(cx, 352000) == 0 && wlan_regulator_validate(cx, 951999) == 0);
    CHECK(wlan_regulator_validate(cx, 351999) == -22 && wlan_regulator_validate(cx, 952001) == -22);
    CHECK(wlan_regulator_find("vdd-1.3-rfa")->min_uv == 1144000);
    CHECK(wlan_regulator_find("vdd-1.8-xo")->max_uv == 1984000);
    CHECK(wlan_regulator_find("nope") == NULL);

    // SMMU and MSA math.
    CHECK(wlan_msa_pages(WLAN_MSA_BASE, WLAN_MSA_SIZE) == 512);
    CHECK(wlan_msa_pages(WLAN_MSA_BASE + 1, WLAN_MSA_SIZE) == 0);
    CHECK(wlan_msa_pages(WLAN_MSA_BASE, 0) == 0);
    CHECK(wlan_msa_pages(~0ULL - 0xfff, 0x2000) == 0);
    uint16_t sid = 0;
    CHECK(wlan_smmu_stream_id(WLAN_APPS_SMMU_PHANDLE, WLAN_SMMU_SID, WLAN_SMMU_SID_MASK, &sid) == 0);
    CHECK(sid == WLAN_SMMU_SID);
    CHECK(wlan_smmu_stream_id(0x19, WLAN_SMMU_SID, 1, &sid) == -22);
    CHECK(wlan_smmu_stream_id(WLAN_APPS_SMMU_PHANDLE, WLAN_SMMU_SID, 0x8000, &sid) == -22);
    CHECK(wlan_smmu_smr(WLAN_SMMU_SID, WLAN_SMMU_SID_MASK) == 0x000100c0u);

    // IRQ table from the DT (raw SPI numbers).
    CHECK(wlan_icnss_irq_spi[0] == 0x84 && wlan_icnss_irq_spi[11] == 0x57);
    CHECK(wlan_icnss_irq_spi[2] == 0x19f && wlan_icnss_irq_spi[4] == 0x1a1);

    // Transport conclusion.
    CHECK(wlan_transport_classify(1, 0, 0, 1) == WLAN_TRANSPORT_SNOC);
    CHECK(wlan_transport_classify(1, 1, 0, 1) == WLAN_TRANSPORT_PCIE);
    CHECK(wlan_transport_classify(1, 0, 1, 1) == WLAN_TRANSPORT_SDIO);
    CHECK(wlan_transport_classify(0, 0, 0, 1) == WLAN_TRANSPORT_NONE);
    CHECK(strcmp(wlan_transport_name(WLAN_BOARD_TRANSPORT), "snoc") == 0);

    // Synthetic MBN: parse, verify, corrupt, truncate, overlap, split.
    uint8_t *mbn = make_mbn();
    struct wlan_mbn m;
    CHECK(wlan_mbn_parse(&m, mbn, T_FILE_LEN) == 0);
    CHECK(m.segments == 3 && m.machine == 40 && m.hash_index == 0 && m.hash_count == 2);
    CHECK(m.payload_size == T_PAY1_LEN + T_PAY2_LEN);
    CHECK(m.image_extent == T_PAY2_OFF + T_PAY2_LEN);
    CHECK(wlan_mbn_verify_hashes(&m, mbn, T_FILE_LEN) == 0);

    mbn[T_PAY1_OFF] ^= 1;
    CHECK(wlan_mbn_verify_hashes(&m, mbn, T_FILE_LEN) == WLAN_ERR_IO);
    mbn[T_PAY1_OFF] ^= 1;
    uint8_t saved = mbn[T_HASH_OFF];
    mbn[T_HASH_OFF] = (uint8_t)(T_PAY1_LEN - 1);
    CHECK(wlan_mbn_verify_hashes(&m, mbn, T_FILE_LEN) == WLAN_ERR_INVAL);
    mbn[T_HASH_OFF] = saved;

    CHECK(wlan_mbn_parse(&m, mbn, T_PAY1_OFF + 32) == WLAN_ERR_INVAL);   // truncated payload
    mbn[0] = 'X';
    CHECK(wlan_mbn_parse(&m, mbn, T_FILE_LEN) == WLAN_ERR_INVAL);
    mbn[0] = 0x7f;
    put16(mbn, 18, 3);                                                    // bad e_machine
    CHECK(wlan_mbn_parse(&m, mbn, T_FILE_LEN) == WLAN_ERR_INVAL);
    put16(mbn, 18, 40);
    mbn[4] = 2;                                                           // ELFCLASS64 not supported
    CHECK(wlan_mbn_parse(&m, mbn, T_FILE_LEN) == WLAN_ERR_INVAL);
    mbn[4] = 1;
    put16(mbn, 44, 0);                                                    // no program headers
    CHECK(wlan_mbn_parse(&m, mbn, T_FILE_LEN) == WLAN_ERR_INVAL);
    put16(mbn, 44, T_PHNUM);
    put32(mbn, T_PHOFF + 2 * 32 + 4, T_PAY1_OFF);                         // overlap
    CHECK(wlan_mbn_parse(&m, mbn, T_FILE_LEN) == WLAN_ERR_INVAL);
    put32(mbn, T_PHOFF + 2 * 32 + 4, T_PAY2_OFF);

    const uint8_t *parts[WLAN_MBN_MAX_SEGMENTS] = {mbn, mbn + T_PAY1_OFF, mbn + T_PAY2_OFF};
    const size_t part_sizes[WLAN_MBN_MAX_SEGMENTS] = {0, T_PAY1_LEN, T_PAY2_LEN};
    struct wlan_mbn split;
    CHECK(wlan_mbn_parse_split(&split, mbn, T_FILE_LEN, parts, part_sizes, 3) == 0);
    CHECK(split.segments == 3 && split.payload_size == T_PAY1_LEN + T_PAY2_LEN);
    CHECK(wlan_mbn_verify_hashes_split(&split, parts, part_sizes, 3) == 0);
    const size_t short_parts[WLAN_MBN_MAX_SEGMENTS] = {0, T_PAY1_LEN - 1, T_PAY2_LEN};
    CHECK(wlan_mbn_parse_split(&split, mbn, T_FILE_LEN, parts, short_parts, 3) == WLAN_ERR_INVAL);
    uint8_t *bad = malloc(T_PAY1_LEN);
    CHECK(bad);
    memcpy(bad, mbn + T_PAY1_OFF, T_PAY1_LEN);
    bad[0] ^= 1;
    const uint8_t *bad_parts[3] = {mbn, bad, mbn + T_PAY2_OFF};
    CHECK(wlan_mbn_parse_split(&split, mbn, T_FILE_LEN, bad_parts, part_sizes, 3) == WLAN_ERR_INVAL);
    free(bad);

    // A trailing self-hash entry (hash_count == segments) is accepted; only the per-segment digests are checked.
    put32(mbn, T_HASH_OFF + 2 * WLAN_MBN_HASH_ENTRY, 0);
    memset(mbn + T_HASH_OFF + 2 * WLAN_MBN_HASH_ENTRY + 4, 0, WLAN_MBN_HASH_LEN);
    put32(mbn, T_PHOFF + 0 * 32 + 20, T_HASH_BYTES + WLAN_MBN_HASH_ENTRY);
    CHECK(wlan_mbn_parse(&m, mbn, T_FILE_LEN) == 0 && m.hash_count == 3);
    CHECK(wlan_mbn_verify_hashes(&m, mbn, T_FILE_LEN) == 0);
    put32(mbn, T_PHOFF + 0 * 32 + 20, T_HASH_BYTES);

    // Unsigned image: removing the hash segment's memsz drops hash checking, and hashes are then a no-op.
    put32(mbn, T_PHOFF + 0 * 32 + 20, 0);
    CHECK(wlan_mbn_parse(&m, mbn, T_FILE_LEN) == 0 && m.hash_index == WLAN_MBN_NO_HASH);
    CHECK(wlan_mbn_verify_hashes(&m, mbn, T_FILE_LEN) == 0);
    put32(mbn, T_PHOFF + 0 * 32 + 20, T_HASH_BYTES);
    CHECK(wlan_mbn_parse(&m, mbn, T_FILE_LEN) == 0);

    // Board-data container and file names.
    struct wlan_bdf bdf;
    CHECK(wlan_bdf_parse(&bdf, mbn, T_FILE_LEN) == 0 && bdf.kind == WLAN_BDF_ELF);
    uint8_t raw[256];
    memset(raw, 0x5a, sizeof raw);
    CHECK(wlan_bdf_parse(&bdf, raw, sizeof raw) == 0 && bdf.kind == WLAN_BDF_RAW);
    CHECK(wlan_bdf_parse(&bdf, raw, 6) == WLAN_ERR_INVAL);       // not word sized
    CHECK(wlan_bdf_parse(&bdf, raw, 0) == WLAN_ERR_INVAL);
    static uint8_t huge[4];
    CHECK(wlan_bdf_parse(&bdf, huge, WLAN_BDF_MAX_SIZE + 4u) == WLAN_ERR_INVAL);
    char name[32];
    CHECK(wlan_bdf_name(name, sizeof name, 0x390, 0x0a, WLAN_BDF_NAME_DEFAULT) == 0 &&
          strcmp(name, "bdwlan.bin") == 0);
    CHECK(wlan_bdf_name(name, sizeof name, 0x390, 0x0a, WLAN_BDF_NAME_BOARD) == 0 &&
          strcmp(name, "bdwlan.b0a") == 0);
    CHECK(wlan_bdf_name(name, sizeof name, 0x390, 0x0a, WLAN_BDF_NAME_CHIP) == 0 &&
          strcmp(name, "bdwlan.390") == 0);
    CHECK(wlan_bdf_name(name, sizeof name, 0x390, 0x0a, WLAN_BDF_NAME_CHIP_BOARD) == 0 &&
          strcmp(name, "bdwlan.390.b0a") == 0);
    CHECK(wlan_bdf_name(name, 4, 0x390, 0x0a, WLAN_BDF_NAME_CHIP_BOARD) == WLAN_ERR_INVAL);
    CHECK(wlan_bdf_name(name, sizeof name, 0x1000, 0x0a, WLAN_BDF_NAME_CHIP) == WLAN_ERR_INVAL);
    CHECK(wlan_bdf_name(name, sizeof name, 0x390, 0x100, WLAN_BDF_NAME_BOARD) == WLAN_ERR_INVAL);

    smem_tests();
    handshake_tests(&m);
    probe_tests();
    real_mdt_test(argc > 1 ? argv[1] : NULL);

    free(mbn);
    printf("PASS: WLAN board math, IRQ/SMMU/MSA facts, transport classification, firmware inventory, ELF/MBN "
           "parse+hash+split, BDF container/names, WLFW handshake and staged probe; synthetic data, no hardware\n");
    return 0;
}
