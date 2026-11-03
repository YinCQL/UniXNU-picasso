// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Picasso's rectangular status bar uses UIKit's native Pad_ForcedNoCell
// provider (selected in the Data seed), with no carrier and time at the left.
// Reserve another 14.5 mm at the right for the panel's camera area. Hook only
// this provider's metric; UIKit still owns item placement and animations.
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

typedef NSDirectionalEdgeInsets (*PicassoEdgeInsets)(id, SEL);
typedef CGFloat (*PicassoMetric)(id, SEL);
static PicassoEdgeInsets original_edge_insets;
static PicassoMetric original_height, original_baseline;

static void picasso_ack(const char *name)
{
    int value = 0;
    size_t size = sizeof(value);
    (void)sysctlbyname(name, &value, &size, NULL, 0);
}

// DevicesOverlay.apk: 34.259979 px / 3, plus the requested 2 pt downward shift.
static const CGFloat picasso_top_padding = 13.42;

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
    static unsigned acknowledged;
    if (!__atomic_exchange_n(&acknowledged, 1, __ATOMIC_RELAXED))
        picasso_ack("kern.picasso_metrics_used");
    NSDirectionalEdgeInsets insets = original_edge_insets(receiver, selector);
    // Vendor 26.729980 px / 3, plus the requested 2 pt clock shift to the right.
    insets.leading = 10.91;
    // Pad's native trailing margin is 5 pt; the previous LegacyPhone margin
    // was 5.5 pt. Keep that origin and move the battery another 4 pt left.
    insets.trailing += 4.5 + picasso_statusbar_offset(UIScreen.mainScreen.bounds.size);
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

static BOOL picasso_install_statusbar(void)
{
    Class provider = objc_getClass("_UIStatusBarVisualProvider_Pad_ForcedNoCell");
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
    return YES;
}

__attribute__((constructor))
static void picasso_sb_probe(void)
{
    BOOL installed = picasso_install_statusbar();
    // Either logging path can be denied by a process sandbox. A kernel exec
    // environment message alone does not establish that this constructor ran.
    picasso_ack("kern.picasso_inject_ack");
    picasso_ack(installed ? "kern.picasso_metrics_installed" : "kern.picasso_metrics_unavailable");

    const char *line = installed ? "PICASSO_SBINJECT statusbar metrics installed (left 10.91 pt, down 13.42 pt, battery shift 14.5 mm + 4 pt)\n"
                                 : "PICASSO_SBINJECT statusbar metric unavailable\n";
    int fd = open("/dev/console", O_WRONLY);
    if (fd >= 0) {
        (void)write(fd, line, strlen(line));
        close(fd);
    }
}
