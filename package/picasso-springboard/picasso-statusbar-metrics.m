// SPDX-License-Identifier: BSD-2-Clause-Patent
// Included after UIKit and the board's acknowledgement helper. Kept separate
// so host tests can exercise real Objective-C class dispatch and late loading.
typedef NSDirectionalEdgeInsets (*PicassoEdgeInsets)(id, SEL);
typedef CGFloat (*PicassoMetric)(id, SEL);
static PicassoEdgeInsets original_edge_insets;
static PicassoMetric original_height, original_baseline;

// Original DevicesOverlay.apk top offset: 34.259979 px / 3.
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
    static unsigned acknowledged;
    if (!__atomic_exchange_n(&acknowledged, 1, __ATOMIC_RELAXED))
        picasso_ack("kern.picasso_metrics_used");
    NSDirectionalEdgeInsets insets = original_edge_insets(receiver, selector);
    // Original vendor leading offset: 26.729980 px / 3.
    insets.leading = 8.91;
    // Pad's native trailing margin is 5 pt; the previous LegacyPhone margin
    // was 5.5 pt. Preserve that origin for the requested 14.5 mm shift.
    insets.trailing += 0.5 + picasso_statusbar_offset(UIScreen.mainScreen.bounds.size);
    return insets;
}

static CGFloat picasso_height(id receiver, SEL selector)
{
    static unsigned acknowledged;
    if (!__atomic_exchange_n(&acknowledged, 1, __ATOMIC_RELAXED))
        picasso_ack("kern.picasso_metrics_height_used");
    return original_height(receiver, selector) + picasso_top_padding;
}

static CGFloat picasso_baseline(id receiver, SEL selector)
{
    static unsigned acknowledged;
    if (!__atomic_exchange_n(&acknowledged, 1, __ATOMIC_RELAXED))
        picasso_ack("kern.picasso_metrics_baseline_used");
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
    // Calls are serialized at startup and on the main queue. Do not capture
    // our own IMP on a retry: that would recurse or accumulate the offsets.
    static BOOL installed;
    if (installed) return YES;
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
    installed = YES;
    return YES;
}
