// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Picasso's rectangular status bar uses UIKit's native Pad_ForcedNoCell
// provider (selected in the Data seed), with no carrier and time at the left.
// Reserve another 14.5 mm at the right for the panel's camera area. Hook only
// this provider's metric; UIKit still owns item placement and animations.
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

typedef NSDirectionalEdgeInsets (*PicassoEdgeInsets)(id, SEL);
typedef CGFloat (*PicassoMetric)(id, SEL);
static PicassoEdgeInsets original_edge_insets;
static PicassoMetric original_height, original_baseline;

// DevicesOverlay.apk: 34.259979 px top padding / current 3x scale.
static const CGFloat picasso_top_padding = 11.42;

static CGFloat picasso_statusbar_offset(CGSize size)
{
    // G7A panel: approximately 69.5 x 154.4 mm, 1080 x 2400 pixels.
    // Vendor DTS reports 695 x 1544 in fields named mm (a tenfold metadata
    // error); do not use those values as literal millimetres. At 360 x 800
    // points this moves the battery 75.1 points, about 225 panel pixels.
    CGFloat millimetres = size.width > size.height ? 154.4 : 69.5;
    return size.width > 0 && size.height > 0 ? size.width * 14.5 / millimetres : 0;
}

static NSDirectionalEdgeInsets picasso_edge_insets(id receiver, SEL selector)
{
    NSDirectionalEdgeInsets insets = original_edge_insets(receiver, selector);
    // 26.729980 panel pixels puts the clock slightly inside the rounded corner.
    insets.leading = 8.91;
    // Pad's native trailing margin is 5 pt; the previous LegacyPhone margin
    // was 5.5 pt. Keep that origin before applying the requested extra shift.
    insets.trailing += 0.5 + picasso_statusbar_offset(UIScreen.mainScreen.bounds.size);
    return insets;
}

static CGFloat picasso_height(id receiver, SEL selector)
{
    return original_height(receiver, selector) + picasso_top_padding;
}

static CGFloat picasso_baseline(id receiver, SEL selector)
{
    return original_baseline(receiver, selector) + picasso_top_padding;
}

static BOOL picasso_metric_compatible(Method method, const char *expected)
{
    if (!method || method_getNumberOfArguments(method) != 2) return NO;
    char *return_type = method_copyReturnType(method);
    BOOL compatible = return_type && !strcmp(return_type, expected);
    free(return_type);
    return compatible;
}

// 21H461's __UIStatusBarGetVisualProviderClassForScreen consults
// UIStatusBarVisualProviderClassName only when -[UIScreen _isEmbeddedScreen]
// returns YES. The generated Picasso screen is not marked embedded, so the
// lookup falls through to the geometry table, prints rdar://45025538 and picks
// _UIStatusBarVisualProvider_Fallback (the red bar). Report the built-in panel
// as embedded and answer the one preference the lookup reads, independently of
// which cfprefsd container the UIKit suite would otherwise use.
static void picasso_marker(const char *name);

static BOOL (*picasso_original_is_embedded)(id, SEL);
static id (*picasso_original_object_for_key)(id, SEL, id);

// This library is arm64 but SpringBoard and UIKitCore are arm64e, so every
// -[UIScreen _isEmbeddedScreen] call delivers a pointer-authenticated LR. Read
// as plain arm64, the PAC fills the high bits and neither dladdr nor an offset
// comparison sees the real return site (wf181-wf184: hook ran, never matched).
static uintptr_t picasso_strip_pointer(uintptr_t pointer)
{
    return pointer & 0x00007fffffffffffULL;
}

// Resolve UIKitCore from the class that owns the selector instead of trusting
// dladdr's view of the shared cache.
static uintptr_t picasso_uikitcore_base(void)
{
    static uintptr_t base;
    if (base) return base;
    Class screen = objc_getClass("UIScreen");
    const char *path = screen ? class_getImageName(screen) : NULL;
    if (!path) return 0;
    for (uint32_t index = 0, count = _dyld_image_count(); index < count; index++) {
        const char *name = _dyld_get_image_name(index);
        if (!name) continue;
        if (strcmp(name, path) == 0 ||
            (strstr(name, "UIKitCore") && strstr(path, "UIKitCore"))) {
            base = (uintptr_t)_dyld_get_image_header(index);
            break;
        }
    }
    return base;
}

static BOOL picasso_is_embedded(id self, SEL _cmd)
{
    // One-shot diagnostic: record what the unmodified answer and the two inputs
    // to -[UIScreen _isEmbeddedScreen] were at the first call (byte +0xcd is the
    // main-display flag UIKit writes at init; the rest is !_isExternal). Marker
    // names are read back from the kernel's open probe.
    static int picasso_embedded_probe;
    if (!picasso_embedded_probe) {
        picasso_embedded_probe = 1;
        BOOL original = picasso_original_is_embedded ? picasso_original_is_embedded(self, _cmd) : NO;
        unsigned main = ((const uint8_t *)(__bridge void *)self)[0xcd] & 1u;
        char name[64];
        snprintf(name, sizeof name, "embedded-orig%u-main%u", original ? 1u : 0u, main);
        picasso_marker(name);
    }
    // -[UIScreen _isEmbeddedScreen] from 0x18a10c3ac and reads the provider
    // preference at the return site 0x18a10c3b0. Answer YES only for that
    // call; the lookup also runs from display plumbing that must keep the
    // generated panel's real answer. UIKitCore's __TEXT starts at
    // 0x189fe6000, so accept any return site inside the lookup function
    // (0x126360..0x1266c4 from the image base).
    uintptr_t caller = picasso_strip_pointer((uintptr_t)__builtin_return_address(0));
    uintptr_t base = picasso_uikitcore_base();
    if (base) {
        uintptr_t offset = caller - base;
        if (offset >= 0x126360 && offset < 0x1266c4) return YES;
    }
    Dl_info info;
    if (caller && dladdr((void *)caller, &info) && info.dli_fbase) {
        uintptr_t offset = caller - (uintptr_t)info.dli_fbase;
        if (offset >= 0x126360 && offset < 0x1266c4) return YES;
    }
    return picasso_original_is_embedded ? picasso_original_is_embedded(self, _cmd) : NO;
}

static id picasso_object_for_key(id self, SEL _cmd, id key)
{
    if ([key isKindOfClass:[NSString class]] &&
        [(NSString *)key isEqualToString:@"UIStatusBarVisualProviderClassName"]) {
        return @"_UIStatusBarVisualProvider_Pad_ForcedNoCell";
    }
    return picasso_original_object_for_key ? picasso_original_object_for_key(self, _cmd, key) : nil;
}

static BOOL picasso_install_selection(void)
{
    Class screen = objc_getClass("UIScreen");
    SEL embedded_selector = sel_registerName("_isEmbeddedScreen");
    Method embedded = screen ? class_getInstanceMethod(screen, embedded_selector) : NULL;
    Class defaults = objc_getClass("NSUserDefaults");
    Method object = defaults ? class_getInstanceMethod(defaults, @selector(objectForKey:)) : NULL;
    // Idempotent: the retries must never capture our own IMP as the original.
    if (embedded && method_getImplementation(embedded) != (IMP)picasso_is_embedded) {
        picasso_original_is_embedded = (BOOL (*)(id, SEL))method_getImplementation(embedded);
        method_setImplementation(embedded, (IMP)picasso_is_embedded);
    }
    if (object && method_getImplementation(object) != (IMP)picasso_object_for_key) {
        picasso_original_object_for_key = (id (*)(id, SEL, id))method_getImplementation(object);
        method_setImplementation(object, (IMP)picasso_object_for_key);
    }
    return embedded && object &&
        method_getImplementation(embedded) == (IMP)picasso_is_embedded &&
        method_getImplementation(object) == (IMP)picasso_object_for_key;
}

// WallpaperKit rejects every 7570 bundle because the generated 360x780 phone
// screen has no matching logical screen class (the ~ipad bundles name
// 1024w-1366h@2x) and because the iPad product types do not include
// iPhone13,1. Both WKAdditions getters exist for exactly this override, but
// they only answer on internal installs; answer them here instead.
static BOOL picasso_ignore_wallpaper_class(id self, SEL _cmd)
{
    (void)self;
    (void)_cmd;
    return YES;
}

static BOOL picasso_install_wallpaper_ignore(void)
{
    Class defaults = objc_getClass("NSUserDefaults");
    Method logical = defaults ? class_getInstanceMethod(defaults,
        sel_registerName("wk_ignoreLogicalScreenClassForWallpaperBundle")) : NULL;
    Method product = defaults ? class_getInstanceMethod(defaults,
        sel_registerName("wk_ignoreProductTypesForWallpaperBundle")) : NULL;
    if (logical && method_getImplementation(logical) != (IMP)picasso_ignore_wallpaper_class) {
        method_setImplementation(logical, (IMP)picasso_ignore_wallpaper_class);
    }
    if (product && method_getImplementation(product) != (IMP)picasso_ignore_wallpaper_class) {
        method_setImplementation(product, (IMP)picasso_ignore_wallpaper_class);
    }
    return logical && product &&
        method_getImplementation(logical) == (IMP)picasso_ignore_wallpaper_class &&
        method_getImplementation(product) == (IMP)picasso_ignore_wallpaper_class;
}

static BOOL picasso_install_statusbar(void)
{
    static BOOL installed;
    if (installed) return YES;
    Class provider = objc_getClass("_UIStatusBarVisualProvider_Pad");
    Method method = class_getClassMethod(provider, @selector(edgeInsets));
    Method height = class_getClassMethod(provider, @selector(height));
    Method baseline = class_getClassMethod(provider, @selector(baselineOffset));
    // 20H392 returns NSDirectionalEdgeInsets in d0-d3, not CGRect or an
    // instance metric. Refuse an incompatible UIKit rather than guessing ABI.
    if (!picasso_metric_compatible(method, @encode(NSDirectionalEdgeInsets)) ||
        !picasso_metric_compatible(height, @encode(CGFloat)) ||
        !picasso_metric_compatible(baseline, @encode(CGFloat))) return NO;
    original_edge_insets = (PicassoEdgeInsets)method_getImplementation(method);
    original_height = (PicassoMetric)method_getImplementation(height);
    original_baseline = (PicassoMetric)method_getImplementation(baseline);
    class_replaceMethod(object_getClass(provider), @selector(edgeInsets),
                        (IMP)picasso_edge_insets, method_getTypeEncoding(method));
    class_replaceMethod(object_getClass(provider), @selector(height),
                        (IMP)picasso_height, method_getTypeEncoding(height));
    class_replaceMethod(object_getClass(provider), @selector(baselineOffset),
                        (IMP)picasso_baseline, method_getTypeEncoding(baseline));
    installed = YES;
    return YES;
}

// Kernel-visible markers: the picasso-inject-open-probe patch logs the path
// copy of every open whose basename starts with "picasso-marker-", before the
// sandbox verdict. That makes them proof that this constructor ran and whether
// the runtime accepted the swizzles, without relying on /dev/console or sysctl.
static void picasso_marker(const char *name)
{
    char path[128];
    snprintf(path, sizeof path, "/private/var/ngpu/picasso-marker-%s", name);
    int fd = open(path, O_RDONLY);
    if (fd >= 0) close(fd);
}

// One installation attempt plus the kernel-visible markers. Tags distinguish
// the constructor from the retries, so the log shows whether UIKit's classes
// were reachable at library-initializer time or only after the run loop starts.
static void picasso_try_installs(const char *tag)
{
    char name[64];
    BOOL installed = picasso_install_statusbar();
    BOOL selection = picasso_install_selection();
    BOOL wallpaper = picasso_install_wallpaper_ignore();
    Class screen = objc_getClass("UIScreen");
    Method embedded = screen ? class_getInstanceMethod(screen, sel_registerName("_isEmbeddedScreen")) : NULL;
    Method object = class_getInstanceMethod(objc_getClass("NSUserDefaults"), @selector(objectForKey:));
    snprintf(name, sizeof name, "%suikit-%s", tag, screen ? "ok" : "missing");
    picasso_marker(name);
    snprintf(name, sizeof name, "%sswizzle-%s", tag,
        embedded && method_getImplementation(embedded) == (IMP)picasso_is_embedded ? "ok" : "fail");
    picasso_marker(name);
    snprintf(name, sizeof name, "%sobject-%s", tag,
        object && method_getImplementation(object) == (IMP)picasso_object_for_key ? "ok" : "fail");
    picasso_marker(name);
    snprintf(name, sizeof name, "%smetrics-%s", tag, installed ? "ok" : "fail");
    picasso_marker(name);
    snprintf(name, sizeof name, "%swallpaper-%s", tag, wallpaper ? "ok" : "fail");
    picasso_marker(name);
    (void)selection;
}

// The toolchain emits this constructor in __TEXT,__init_offsets. Matching
// dyld-1165.3 source enumerates that section, while +load gives an independent
// runtime entry point if the image is mapped. Install from both; the operations
// are idempotent.
@interface PicassoSBInject : NSObject
@end
@implementation PicassoSBInject
+ (void)load
{
    extern void picasso_sb_probe(void);
    picasso_sb_probe();
}
@end

__attribute__((constructor))
void picasso_sb_probe(void)
{
    picasso_marker("constructor");
    picasso_try_installs("");
    // An inserted library's initializer can run before UIKit has realized the
    // classes it swizzles; the status bar provider is chosen when the first
    // status bar is created, so a late retry still wins. Every step is
    // idempotent and re-scans the method table.
    static const int delays[] = { 1, 5, 15 };
    static const char *const tags[] = { "late1-", "late2-", "late3-" };
    for (unsigned index = 0; index < sizeof(delays) / sizeof(delays[0]); ++index) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)delays[index] * NSEC_PER_SEC),
            dispatch_get_main_queue(), ^{
                picasso_marker(tags[index]);
                picasso_try_installs(tags[index]);
            });
    }
    int value = 0;
    size_t size = sizeof(value);
    // Either logging path can be denied by a process sandbox. A kernel exec
    // environment message alone does not establish that this constructor ran.
    sysctlbyname("kern.picasso_inject_ack", &value, &size, NULL, 0);

    // The sandbox may deny both of these; the kernel path-copy markers above
    // are the authoritative signal.
    const char *line = "PICASSO_SBINJECT constructor ran (selection+metrics+wallpaper attempted; markers in kernel log)\n";
    int fd = open("/dev/console", O_WRONLY);
    if (fd >= 0) {
        (void)write(fd, line, strlen(line));
        close(fd);
    }
}
