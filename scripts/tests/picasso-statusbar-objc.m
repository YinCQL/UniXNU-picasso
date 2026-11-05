// SPDX-License-Identifier: BSD-2-Clause-Patent
// Real Objective-C dispatch on macOS with modeled UIKit providers/screen.
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <objc/runtime.h>
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct NSDirectionalEdgeInsets { CGFloat top, leading, bottom, trailing; } NSDirectionalEdgeInsets;
static CGSize screen_size;
@interface UIScreen : NSObject
+ (UIScreen *)mainScreen;
- (CGRect)bounds;
@end
@implementation UIScreen
+ (UIScreen *)mainScreen { static UIScreen *screen; if (!screen) screen = [UIScreen new]; return screen; }
- (CGRect)bounds { return (CGRect){CGPointZero, screen_size}; }
@end

static unsigned acknowledgements;
static void picasso_ack(const char *name)
{
    assert(strstr(name, "kern.picasso_metrics_") == name);
    ++acknowledgements;
}
#include "picasso-statusbar-metrics.m"

@interface PicassoPadFixture : NSObject
+ (NSDirectionalEdgeInsets)edgeInsets;
+ (CGFloat)height;
+ (CGFloat)baselineOffset;
@end
@implementation PicassoPadFixture
+ (NSDirectionalEdgeInsets)edgeInsets { return (NSDirectionalEdgeInsets){1, 6, 2, 5}; }
+ (CGFloat)height { return 20; }
+ (CGFloat)baselineOffset { return 14; }
@end
@interface PicassoPadSibling : PicassoPadFixture @end
@implementation PicassoPadSibling @end
static int wrong_metric(id receiver, SEL selector) { (void)receiver; (void)selector; return 14; }
static void near(CGFloat actual, CGFloat expected) { assert(fabs(actual - expected) < 1e-8); }

int main(int argc, const char **argv)
{
    @autoreleasepool {
        assert(!picasso_install_statusbar()); // Missing at first attempt.
        Class provider = objc_allocateClassPair([PicassoPadFixture class],
            "_UIStatusBarVisualProvider_Pad_ForcedNoCell", 0);
        assert(provider);
        if (argc > 1) {
            const char *type = !strcmp(argv[1], "return") ? "i16@0:8" : "d24@0:8i16";
            assert(class_addMethod(object_getClass(provider), @selector(baselineOffset), (IMP)wrong_metric, type));
        }
        objc_registerClassPair(provider);
        // Warm the inherited method caches before replacement.
        near([provider height], 20); near([provider edgeInsets].leading, 6);
        IMP parent = method_getImplementation(class_getClassMethod([PicassoPadFixture class], @selector(edgeInsets)));
        if (argc > 1) {
            assert(!picasso_install_statusbar());
            assert(method_getImplementation(class_getClassMethod(provider, @selector(edgeInsets))) == parent);
            assert(!acknowledgements);
            puts("PASS: incompatible metric ABI rejects the entire install without modifying methods");
            return 0;
        }
        assert(picasso_install_statusbar()); // Retry after class registration.
        assert(!acknowledgements); // Installation does not fake actual UIKit usage.
        screen_size = CGSizeMake(360, 800);
        for (unsigned i = 0; i < 100; ++i) {
            assert(picasso_install_statusbar()); // No recapture of our wrappers.
            NSDirectionalEdgeInsets insets = [provider edgeInsets];
            near(insets.top, 1); near(insets.bottom, 2);
            near(insets.leading, 8.91); near(insets.trailing, 80.60791366906475);
            near([provider height], 31.42); near([provider baselineOffset], 25.42);
        }
        assert(acknowledgements == 3);
        near([PicassoPadFixture edgeInsets].leading, 6); near([PicassoPadSibling edgeInsets].trailing, 5);
        near([PicassoPadSibling height], 20); near([PicassoPadFixture baselineOffset], 14);
        assert(method_getImplementation(class_getClassMethod([PicassoPadFixture class], @selector(edgeInsets))) == parent);
        screen_size = CGSizeMake(800, 360);
        near([provider edgeInsets].trailing, 80.62953367875648);
        near(picasso_statusbar_offset(CGSizeMake(1080, 2400)) / 3, 75.10791366906475);
        near(picasso_statusbar_offset(CGSizeMake(0, 800)), 0);
        near(picasso_statusbar_offset(CGSizeMake(360, 0)), 0);
        near(picasso_statusbar_offset(CGSizeMake(-1, 800)), 0);
        puts("PASS: late class, warm inherited dispatch, repeat install, original superclass, portrait/landscape and requested 8.91/11.42/14.5 metrics");
    }
    return 0;
}
