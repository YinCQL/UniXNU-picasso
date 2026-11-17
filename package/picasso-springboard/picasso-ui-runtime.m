// SPDX-License-Identifier: BSD-2-Clause-Patent
// Keep the independently verified seed source intact. This signed runtime
// build includes its status-bar/ASTC behavior plus the input cache and power-button corrections.
#include "picasso-springboard-probe.m"
#include "picasso-input-cache.m"
#include "picasso-keyboard-size.m"
#include "picasso-power-button.m"
#include "picasso-snapshot-resample.m"
#include "picasso-snapshot-store.m"

/* Observe the stock Bluetooth footer query in Preferences only. The original
 * method and its result remain unchanged. Framework UUID, method entry and
 * argument count must match the exact 20H392 implementation before installing.
 * Report presence/types/status and name equality, never the user's name or a
 * complete preference dictionary. These probes perform no configuration writes. */
#include <dlfcn.h>
#include <mach-o/loader.h>
#include <stdatomic.h>

typedef const void *picasso_bt_store_ref;
typedef const void *picasso_bt_prefs_ref;
static id (*picasso_bt_footer_original)(id, SEL, id, NSInteger);
static CFStringRef (*picasso_bt_copy_name)(picasso_bt_store_ref, CFStringEncoding *);
static CFPropertyListRef (*picasso_bt_copy_value)(picasso_bt_store_ref, CFStringRef);
static picasso_bt_prefs_ref (*picasso_bt_create_prefs)(CFAllocatorRef, CFStringRef, CFStringRef);
static CFPropertyListRef (*picasso_bt_path_value)(picasso_bt_prefs_ref, CFStringRef);
static int (*picasso_bt_sc_error)(void);
static CFTypeRef (*picasso_bt_mg_answer)(CFStringRef, CFDictionaryRef);
static atomic_uint picasso_bt_name_samples;

static unsigned picasso_bt_name_type(CFTypeRef value)
{
    if (!value) return 0;
    CFTypeID type = CFGetTypeID(value);
    if (type == CFStringGetTypeID()) return 1;
    if (type == CFDictionaryGetTypeID()) return 2;
    return 3;
}

static unsigned picasso_bt_name_status(int status)
{
    return status >= 0 && status <= 65535 ? (unsigned)status : 65535;
}

static void picasso_bt_name_note(unsigned sample, unsigned part, unsigned a,
    unsigned b, unsigned c, unsigned d)
{
    char path[160];
    int length = snprintf(path, sizeof(path),
        "/private/var/picasso/picasso-marker-btname-%u-%u-%u-%u-%u-%u",
        sample, part, a, b, c, d);
    if (length <= 0 || (size_t)length >= sizeof(path)) return;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) close(fd);
}

static void picasso_bt_name_observe(void)
{
    unsigned sample = atomic_load_explicit(&picasso_bt_name_samples, memory_order_relaxed);
    do {
        if (sample >= 6) return;
    } while (!atomic_compare_exchange_weak_explicit(&picasso_bt_name_samples, &sample,
        sample + 1, memory_order_relaxed, memory_order_relaxed));
    ++sample;
    CFStringRef name = picasso_bt_copy_name(NULL, NULL);
    unsigned name_error = picasso_bt_name_status(picasso_bt_sc_error());
    CFPropertyListRef setup = picasso_bt_copy_value(NULL, CFSTR("Setup:/System"));
    unsigned setup_error = picasso_bt_name_status(picasso_bt_sc_error());
    CFTypeRef setup_name = picasso_bt_name_type(setup) == 2 ?
        CFDictionaryGetValue((CFDictionaryRef)setup, CFSTR("ComputerName")) : NULL;
    picasso_bt_name_note(sample, 1, picasso_bt_name_type(name), name_error,
        picasso_bt_name_type(setup), setup_error);

    picasso_bt_prefs_ref prefs = picasso_bt_create_prefs(NULL,
        CFSTR("Picasso Bluetooth name diagnostic"), NULL);
    CFPropertyListRef system = prefs ? picasso_bt_path_value(prefs, CFSTR("/System/System")) : NULL;
    unsigned prefs_error = picasso_bt_name_status(picasso_bt_sc_error());
    CFTypeRef prefs_name = picasso_bt_name_type(system) == 2 ?
        CFDictionaryGetValue((CFDictionaryRef)system, CFSTR("ComputerName")) : NULL;
    picasso_bt_name_note(sample, 2, picasso_bt_name_type(setup_name), prefs != NULL,
        picasso_bt_name_type(prefs_name), prefs_error);

    CFTypeRef assigned = picasso_bt_mg_answer ?
        picasso_bt_mg_answer(CFSTR("UserAssignedDeviceName"), NULL) : NULL;
    unsigned assigned_type = picasso_bt_name_type(assigned);
    unsigned same_name = assigned_type == 1 && picasso_bt_name_type(name) == 1 ?
        (CFEqual(assigned, name) ? 1 : 2) : 0;
    unsigned same_prefs = assigned_type == 1 && picasso_bt_name_type(prefs_name) == 1 ?
        (CFEqual(assigned, prefs_name) ? 1 : 2) : 0;
    picasso_bt_name_note(sample, 3, assigned_type, same_name, same_prefs,
        picasso_bt_name_type(system));
    if (assigned) CFRelease(assigned);
    if (prefs) CFRelease(prefs);
    if (setup) CFRelease(setup);
    if (name) CFRelease(name);
}

static id picasso_bt_footer(id receiver, SEL selector, id table, NSInteger section)
{
    id footer = picasso_bt_footer_original(receiver, selector, table, section);
    if (section == 0 && [NSThread isMainThread]) picasso_bt_name_observe();
    return footer;
}

static void picasso_bt_name_install(uintptr_t base)
{
    static BOOL installed;
    if (installed || ![[NSProcessInfo processInfo].processName isEqualToString:@"Preferences"]) return;
    Class cls = objc_getClass("BTSDevicesController");
    SEL selector = sel_registerName("tableView:viewForFooterInSection:");
    Method method = class_getInstanceMethod(cls, selector);
    uintptr_t entry = base + 0xeb78;
    static const unsigned char expected_entry[32] = {
        0xff,0x43,0x06,0xd1,0xfc,0x6f,0x13,0xa9,0xfa,0x67,0x14,0xa9,0xf8,0x5f,0x15,0xa9,
        0xf6,0x57,0x16,0xa9,0xf4,0x4f,0x17,0xa9,0xfd,0x7b,0x18,0xa9,0xfd,0x03,0x06,0x91
    };
    if (!method || method_getNumberOfArguments(method) != 4 ||
        (uintptr_t)method_getImplementation(method) != entry ||
        memcmp((const void *)entry, expected_entry, sizeof(expected_entry))) return;
    void *sc = dlopen("/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration", RTLD_LAZY | RTLD_LOCAL);
    if (!sc) return;
    picasso_bt_copy_name = dlsym(sc, "SCDynamicStoreCopyComputerName");
    picasso_bt_copy_value = dlsym(sc, "SCDynamicStoreCopyValue");
    picasso_bt_create_prefs = dlsym(sc, "SCPreferencesCreate");
    picasso_bt_path_value = dlsym(sc, "SCPreferencesPathGetValue");
    picasso_bt_sc_error = dlsym(sc, "SCError");
    if (!picasso_bt_copy_name || !picasso_bt_copy_value || !picasso_bt_create_prefs ||
        !picasso_bt_path_value || !picasso_bt_sc_error) { dlclose(sc); return; }
    void *mg = dlopen("/usr/lib/libMobileGestalt.dylib", RTLD_LAZY | RTLD_LOCAL);
    if (mg) picasso_bt_mg_answer = dlsym(mg, "MGCopyAnswer");
    picasso_bt_footer_original = (void *)method_getImplementation(method);
    method_setImplementation(method, (IMP)picasso_bt_footer);
    installed = YES;
    picasso_bt_name_note(0, 0, 1, mg != NULL, picasso_bt_mg_answer != NULL, 0);
}

static void picasso_bt_name_image_added(const struct mach_header *header, intptr_t slide)
{
    (void)slide;
    if (!header || header->magic != MH_MAGIC_64 || header->ncmds > 128 || header->sizeofcmds > 65536) return;
    static const unsigned char expected[16] = {
        0x43,0x3b,0xd2,0x2d,0x83,0x55,0x34,0x2f,0xba,0x34,0xd2,0x47,0x1d,0xc0,0x1c,0xb0
    };
    const unsigned char *cursor = (const unsigned char *)header + sizeof(struct mach_header_64);
    const unsigned char *end = cursor + header->sizeofcmds;
    BOOL matches = NO;
    uintptr_t base = (uintptr_t)header, text_end = 0;
    for (unsigned i = 0; i < header->ncmds; ++i) {
        if ((size_t)(end - cursor) < sizeof(struct load_command)) return;
        const struct load_command *command = (const struct load_command *)cursor;
        if (command->cmdsize < sizeof(*command) || command->cmdsize > (size_t)(end - cursor)) return;
        if (command->cmd == LC_UUID && command->cmdsize == sizeof(struct uuid_command)) {
            matches = !memcmp(((const struct uuid_command *)command)->uuid, expected, sizeof(expected));
        }
        if (command->cmd == LC_SEGMENT_64 && command->cmdsize >= sizeof(struct segment_command_64)) {
            const struct segment_command_64 *segment = (const struct segment_command_64 *)command;
            uintptr_t begin = segment->vmaddr + slide;
            if (segment->vmsize > UINTPTR_MAX - begin) return;
            if (!strncmp(segment->segname, "__TEXT", 16) && begin == base)
                text_end = begin + segment->vmsize;
        }
        cursor += command->cmdsize;
    }
    if (!matches || cursor != end || text_end < base || text_end - base < 0xeb78 + 32) return;
    // Leave the dyld callback before class lookup and framework loading.
    dispatch_async(dispatch_get_main_queue(), ^{ picasso_bt_name_install(base); });
}


__attribute__((constructor))
static void picasso_input_start(void)
{
    _dyld_register_func_for_add_image(picasso_input_image_added);
    _dyld_register_func_for_add_image(picasso_power_image_added);
    _dyld_register_func_for_add_image(picasso_bt_name_image_added);
}
