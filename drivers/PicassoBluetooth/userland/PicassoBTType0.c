/* bluetoothd (20H392, UUID AA9424E8-0648-34E2-B3CF-E6C793B9D471) type 0
 * (THIRD_PARTY_DONGLE) controller capabilities for Picasso WCN399x.
 * Derived from Renoir Bluetooth, pinned in NOTICE.md.
 *
 * Loaded through an added LC_LOAD_DYLIB; the constructor stores the publish
 * entry in a slot at the tail of bluetoothd's __DATA (0x100867ff0) that the
 * offline-patched trampoline of initCmdReadLeLocalSupportedFeaturesCB
 * (0x10010c114) calls. bluetoothd's code pages are never written at run time.
 *
 * At that point the native HCI init chain has stored Read Local Supported
 * Commands (internal support table 0x100863938, indexed through the command
 * descriptors at 0x1008448e0) and LE Read Local Supported Features
 * (0x1008638e4). Standard capabilities are derived from those reports and
 * published both to the lower-layer selections (the setters initCoreStack
 * used with the pre-probe answers) and to the hardware model (an RW copy of
 * its vtable with these getters replaced), so upper and lower layers choose
 * the same command family. Advertising capacity comes from a real LE Read
 * Number of Supported Advertising Sets (0x203B), inserted into the native
 * chain before it advances; its completion publishes the advertising choice.
 * LeBroadcaster also caches a legacy/multiple mode before HCI discovery. A
 * second offline trampoline refreshes that choice at its real stack-start
 * entry, before native LE_GAP_Init builds the standard advertising sets. */
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <os/log.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

static const uint8_t expected_uuid[16] = {0xaa,0x94,0x24,0xe8,0x06,0x48,0x34,0xe2,0xb3,0xcf,0xe6,0xc7,0x93,0xb9,0xd4,0x71};
extern void picasso_sw_install(intptr_t slide);
static intptr_t slide;
static os_log_t logger;
#define AT(address) ((void *)((uintptr_t)(address) + slide))

/* bluetoothd */
#define SLOT_KEYCHAIN_GET     0x100867fc0
#define SLOT_KEYCHAIN_SET     0x100867fc8
#define ORIGINAL_KEYCHAIN_GET 0x100006000
#define ORIGINAL_KEYCHAIN_SET 0x100006010
#define SLOT_PUBLISH          0x100867ff0
#define SLOT_PREPARE_ADV      0x100867ff8
#define SUPPORT_TABLE         0x100863938   /* 47 bytes, byte:mask per descriptor */
#define SUPPORT_TABLE_BYTES   47
#define LE_FEATURES           0x1008638e4
#define DESCRIPTORS           0x1008448e0   /* 24 bytes: +2 opcode, +4 mask | byte << 8 */
#define DESCRIPTOR_COUNT      0xfd          /* 0x10014a340 checks id < 0xfd */
#define SUBMIT                0x10010ca98   /* (internal id, completion, ...) */
#define ADVANCE               0x1001094ac   /* (status, step name) */
#define CHAIN_FAIL            0x1001c2284   /* (chain context, status): native submit failure */
#define CHAIN_CONTEXT         0x10084f598
#define INIT_SUBMIT_FLAG      0x10086391c   /* set around init-time submits, e.g. 0x10010a934..a948 */
#define FEATURES_STEP_NAME    0x1006edbc0   /* "initCmdReadLeLocalSupportedFeaturesCB" */
#define MODEL                 0x10029aba8
#define SET_EXT_ADV           0x10017763c
#define SET_SPEC_ADV          0x100177630
#define SET_ADV_HANDLES       0x100177658
#define SET_ENH_CONNECTION    0x100188390
#define SET_EXT_SCAN          0x10019a7e8
#define SET_CODED_SCAN        0x10019a920   /* -> 0x10085a641, read by 0x1001998e4 */
#define SET_PERIODIC_ADV      0x100178bbc   /* -> 0x100854b00 */
#define SET_PERIODIC_SYNC     0x10008efec   /* -> 0x10084b7a8 */
#define UPPER_EXT_ADV_CACHE   0x10085cbb8
#define ID_READ_NUM_ADV_SETS  0xd1
#define ADV_SET_ARRAY_CAPACITY 5            /* 7-byte entries, 35 bytes, see audit section 6 */
#define MODEL_MULTIPLE_ADV    0xa0          /* nonvirtual SupportsMultipleAdvertising field */
#define BROADCASTER_MODE      0x88          /* 0: multiple sets, 1: single legacy instance */
#define VTABLE_PREFIX         0x10          /* offset-to-top, typeinfo */
#define VTABLE_BYTES          0xa98

static struct {
    bool ext_adv, ext_scan, enh_conn, per_adv, per_sync, past_send, past_recv, coded;
    uint32_t adv_sets;
} caps;

static bool command_supported(uint16_t opcode)
{
    const uint8_t *table = AT(SUPPORT_TABLE);
    const uint8_t *d = AT(DESCRIPTORS);
    for(unsigned i = 0; i < DESCRIPTOR_COUNT; ++i, d += 24) {
        uint16_t op, bit;
        memcpy(&op, d + 2, 2); memcpy(&bit, d + 4, 2);
        if(op == opcode)
            return (bit >> 8) < SUPPORT_TABLE_BYTES && (table[bit >> 8] & (bit & 0xff));
    }
    return false;
}
static bool commands_supported(const uint16_t *ops, unsigned n)
{
    for(unsigned i = 0; i < n; ++i) if(!command_supported(ops[i])) return false;
    return true;
}

static bool get_ext_adv(void *self) { (void)self; return caps.ext_adv; }
static bool get_ext_scan(void *self) { (void)self; return caps.ext_scan; }
static bool get_enh_conn(void *self) { (void)self; return caps.enh_conn; }
static bool get_per_adv(void *self) { (void)self; return caps.per_adv; }
static bool get_per_sync(void *self) { (void)self; return caps.per_sync; }
static bool get_past_send(void *self) { (void)self; return caps.past_send; }
static bool get_past_recv(void *self) { (void)self; return caps.past_recv; }
static bool get_coded(void *self) { (void)self; return caps.coded; }
static uint32_t get_adv_sets(void *self) { (void)self; return caps.adv_sets; }

static void *model_vtable[(VTABLE_PREFIX + VTABLE_BYTES) / sizeof(void *)];

static void publish_model(void)
{
    void *(*model)(void) = AT(MODEL);
    void ***object = model();
    void **original = *object;
    if(original == model_vtable + VTABLE_PREFIX / sizeof(void *)) return;   /* reopen: getters read caps */
    if(original != (void **)AT(0x100802480)) {
        os_log_error(logger, "type0: model vtable %p is not the audited 0x100802480 class", original);
        return;
    }
    memcpy(model_vtable, (char *)original - VTABLE_PREFIX, sizeof(model_vtable));
    void **v = model_vtable + VTABLE_PREFIX / sizeof(void *);
    v[0x618 / 8] = get_ext_adv;
    v[0x680 / 8] = get_ext_adv;
    v[0x670 / 8] = get_ext_scan;
    v[0x678 / 8] = get_enh_conn;
    v[0x620 / 8] = get_per_adv;
    v[0x628 / 8] = get_per_sync;
    v[0x630 / 8] = get_past_send;
    v[0x638 / 8] = get_past_recv;
    v[0x6f0 / 8] = get_coded;
    v[0x990 / 8] = get_adv_sets;
    *object = v;
}

static void publish_advertising(void)
{
    void *(*model)(void) = AT(MODEL);
    /* 0x100259148..254 normally derives this field from the chip model.
     * Type 0 learns it from the actual 203B response, with the other caps. */
    *((uint8_t *)model() + MODEL_MULTIPLE_ADV) = caps.ext_adv;
    ((void (*)(bool))AT(SET_EXT_ADV))(caps.ext_adv);
    ((void (*)(bool))AT(SET_SPEC_ADV))(caps.ext_adv);
    ((void (*)(uint32_t))AT(SET_ADV_HANDLES))(caps.adv_sets);
    *(uint8_t *)AT(UPPER_EXT_ADV_CACHE) = caps.ext_adv;
    os_log(logger, "type0: extended advertising %d, advertising sets %u", caps.ext_adv, caps.adv_sets);
}

/* 0x10050a18c is the broadcaster's stack-start method. Its early setup at
 * 0x100509670 cached mode 1 before HCI probing. Native 0x10050a230 uses this
 * mode to choose LE_InitMultipleAdvertising; without refreshing it, count
 * stays zero and 0x100176620 sends legacy 2006 after extended 203B.
 * Leave array setup, 203A, command replies and every start/stop to the
 * original method. Run here again on reopen, not by rerunning constructors. */
static void prepare_advertising(void *broadcaster)
{
    uint32_t mode = caps.ext_adv ? 0 : 1;
    memcpy((uint8_t *)broadcaster + BROADCASTER_MODE, &mode, sizeof(mode));
    os_log(logger, "type0: broadcaster mode %u, standard advertising sets %u", mode, caps.adv_sets);
}

static void advance(uint32_t status, const char *step)
{
    ((void (*)(uint32_t, const char *))AT(ADVANCE))(status, step);
}

/* LE Read Number of Supported Advertising Sets completion: (status, sets).
 * A failed query is passed to the native chain with its real status. */
static void read_adv_sets_complete(uint32_t status, uint32_t sets)
{
    if(status) {
        os_log_error(logger, "type0: LE Read Number of Supported Advertising Sets status %u", status);
        advance(status, "initCmdReadNumAdvSets");
        return;
    }
    caps.adv_sets = sets < ADV_SET_ARRAY_CAPACITY ? sets : ADV_SET_ARRAY_CAPACITY;
    if(!caps.adv_sets) caps.ext_adv = false;
    publish_advertising();
    advance(0, "initCmdReadNumAdvSets");
}

/* Replaces the advance at the end of initCmdReadLeLocalSupportedFeaturesCB. */
static void publish(void)
{
    uint64_t f;
    memcpy(&f, AT(LE_FEATURES), sizeof(f));
    static const uint16_t adv[] = {0x2035, 0x2036, 0x2037, 0x2038, 0x2039, 0x203a, 0x203c, 0x203d};
    static const uint16_t scan[] = {0x2041, 0x2042};
    static const uint16_t per_adv[] = {0x203e, 0x203f, 0x2040};
    static const uint16_t per_sync[] = {0x2044, 0x2045, 0x2046};
    static const uint16_t past_send[] = {0x205a, 0x205b};
    static const uint16_t past_recv[] = {0x2059, 0x205c, 0x205d};
    caps.ext_adv = (f >> 12 & 1) && commands_supported(adv, 8) && command_supported(0x203b);
    caps.ext_scan = commands_supported(scan, 2);
    caps.enh_conn = command_supported(0x2043);
    caps.per_adv = (f >> 13 & 1) && commands_supported(per_adv, 3);
    caps.per_sync = (f >> 13 & 1) && commands_supported(per_sync, 3);
    caps.past_send = (f >> 24 & 1) && commands_supported(past_send, 2);
    caps.past_recv = (f >> 25 & 1) && commands_supported(past_recv, 3);
    caps.coded = f >> 11 & 1;
    os_log(logger, "type0: LE features %016llx; extended scan %d, enhanced connection %d, periodic adv %d sync %d, PAST send %d recv %d, coded PHY %d",
        f, caps.ext_scan, caps.enh_conn, caps.per_adv, caps.per_sync, caps.past_send, caps.past_recv, caps.coded);
    ((void (*)(bool))AT(SET_EXT_SCAN))(caps.ext_scan);
    ((void (*)(bool))AT(SET_ENH_CONNECTION))(caps.enh_conn);
    ((void (*)(bool))AT(SET_CODED_SCAN))(caps.coded);
    ((void (*)(bool))AT(SET_PERIODIC_ADV))(caps.per_adv);
    ((void (*)(bool))AT(SET_PERIODIC_SYNC))(caps.per_sync);
    publish_model();
    if(!caps.ext_adv) {
        caps.adv_sets = 0;
        publish_advertising();
        advance(0, AT(FEATURES_STEP_NAME));
        return;
    }
    /* Native init-time submission (as ReadLocalVersionInformation 0x10010a934):
     * flag set, submit, flag cleared right after the synchronous return. */
    volatile uint8_t *flag = AT(INIT_SUBMIT_FLAG);
    *flag = 1;
    uint32_t e = ((uint32_t (*)(uint32_t, void *))AT(SUBMIT))(ID_READ_NUM_ADV_SETS, read_adv_sets_complete);
    *flag = 0;
    if(e) {
        os_log_error(logger, "type0: submitting LE Read Number of Supported Advertising Sets returned %#x", e);
        ((void (*)(void *, uint32_t))AT(CHAIN_FAIL))(*(void **)AT(CHAIN_CONTEXT), e);
    }
}

static void keychain_trace(uint32_t operation,uint32_t kind,int32_t status,uint32_t bytes)
{
    const uint32_t record[4]={operation,kind,(uint32_t)status,bytes};
    (void)sysctlbyname("kern.qbt_keychain_trace",NULL,NULL,(void *)record,sizeof(record));
}
static int32_t keychain_get(uint32_t kind,void *bytes,uint32_t capacity,uint32_t *length)
{
    int32_t result=((int32_t (*)(uint32_t,void *,uint32_t,uint32_t *))AT(ORIGINAL_KEYCHAIN_GET))(kind,bytes,capacity,length);
    keychain_trace(1,kind,result,length?*length:0);return result;
}
static int32_t keychain_set(uint32_t kind,const void *bytes,uint32_t length)
{
    int32_t result=((int32_t (*)(uint32_t,const void *,uint32_t))AT(ORIGINAL_KEYCHAIN_SET))(kind,bytes,length);
    keychain_trace(2,kind,result,length);return result;
}

__attribute__((constructor)) static void type0_init(void)
{
    logger = os_log_create("com.apple.bluetooth", "RenoirType0");
    const struct mach_header_64 *header = (const struct mach_header_64 *)_dyld_get_image_header(0);
    const uint8_t *lc = (const uint8_t *)(header + 1);
    bool match = false;
    for(uint32_t i = 0; i < header->ncmds; ++i) {
        const struct load_command *c = (const struct load_command *)lc;
        if(c->cmd == LC_UUID) match = !memcmp(((const struct uuid_command *)c)->uuid, expected_uuid, 16);
        lc += c->cmdsize;
    }
    if(!match) {
        os_log_error(logger, "type0: main image is not the audited 20H392 bluetoothd; not installed");
        return;
    }
    slide = _dyld_get_image_vmaddr_slide(0);
    picasso_sw_install(slide);
    *(void **)AT(SLOT_KEYCHAIN_GET) = (void *)keychain_get;
    *(void **)AT(SLOT_KEYCHAIN_SET) = (void *)keychain_set;
    *(void **)AT(SLOT_PUBLISH) = (void *)publish;
    *(void **)AT(SLOT_PREPARE_ADV) = (void *)prepare_advertising;
}
