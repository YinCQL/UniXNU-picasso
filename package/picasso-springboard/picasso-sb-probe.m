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
static PicassoEdgeInsets original_edge_insets;

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
    // Pad's native trailing margin is 5 pt; the previous LegacyPhone margin
    // was 5.5 pt. Keep that origin before applying the requested extra shift.
    insets.trailing += 0.5 + picasso_statusbar_offset(UIScreen.mainScreen.bounds.size);
    return insets;
}

static BOOL picasso_install_statusbar(void)
{
    Class provider = objc_getClass("_UIStatusBarVisualProvider_Pad");
    Method method = class_getClassMethod(provider, @selector(edgeInsets));
    if (!method || method_getNumberOfArguments(method) != 2) return NO;
    // 20H392 returns NSDirectionalEdgeInsets in d0-d3, not CGRect or an
    // instance metric. Refuse an incompatible UIKit rather than guessing ABI.
    char *return_type = method_copyReturnType(method);
    BOOL compatible = return_type && !strcmp(return_type, @encode(NSDirectionalEdgeInsets));
    free(return_type);
    if (!compatible) return NO;
    original_edge_insets = (PicassoEdgeInsets)method_getImplementation(method);
    class_replaceMethod(object_getClass(provider), @selector(edgeInsets),
                        (IMP)picasso_edge_insets, method_getTypeEncoding(method));
    return YES;
}

__attribute__((constructor))
static void picasso_sb_probe(void)
{
    BOOL installed = picasso_install_statusbar();
    int value = 0;
    size_t size = sizeof(value);
    // Either logging path can be denied by a process sandbox. A kernel exec
    // environment message alone does not establish that this constructor ran.
    sysctlbyname("kern.picasso_inject_ack", &value, &size, NULL, 0);

    const char *line = installed ? "PICASSO_SBINJECT statusbar metric installed (14.5 mm)\n"
                                 : "PICASSO_SBINJECT statusbar metric unavailable\n";
    int fd = open("/dev/console", O_WRONLY);
    if (fd >= 0) {
        (void)write(fd, line, strlen(line));
        close(fd);
    }
}
