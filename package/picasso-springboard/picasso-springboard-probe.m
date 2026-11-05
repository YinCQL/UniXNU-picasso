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
#include <stdio.h>
#include <dispatch/dispatch.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

// Read-only diagnostics also use a nonexistent marker path. The bounded namei
// probe records the attempt before lookup, so no file needs to be created and
// no sandbox/AMFI policy is relaxed when a sysctl or console open is denied.
static void picasso_ack(const char *name)
{
    int value = 0;
    size_t size = sizeof(value);
    // Height and baseline have marker-only acknowledgements; the original
    // kernel ABI has one generic metrics-used sysctl for edgeInsets.
    if (strcmp(name, "kern.picasso_metrics_height_used") &&
        strcmp(name, "kern.picasso_metrics_baseline_used"))
        (void)sysctlbyname(name, &value, &size, NULL, 0);
    static const char prefix[] = "kern.picasso_";
    if (strncmp(name, prefix, sizeof(prefix) - 1)) return;
    char marker[160];
    int length = snprintf(marker, sizeof(marker), "/private/var/picasso/picasso-marker-%s",
                          name + sizeof(prefix) - 1);
    if (length <= 0 || (size_t)length >= sizeof(marker)) return;
    int fd = open(marker, O_RDONLY);
    if (fd >= 0) close(fd);
}

#include "picasso-astc-guard.m"
#include "picasso-statusbar-metrics.m"

static void picasso_statusbar_start(void)
{
    BOOL installed = picasso_install_statusbar();
    picasso_ack(installed ? "kern.picasso_metrics_installed" : "kern.picasso_metrics_unavailable");
    const char *line = installed ? "PICASSO_SBINJECT statusbar metrics installed (left 8.91 pt, down 11.42 pt, battery shift 14.5 mm)\n"
                                 : "PICASSO_SBINJECT statusbar metric unavailable\n";
    int fd = open("/dev/console", O_WRONLY);
    if (fd >= 0) {
        (void)write(fd, line, strlen(line));
        close(fd);
    }
    if (!installed) {
        // UIKit is a linked dependency, but a missing runtime class must not
        // turn the sole constructor attempt into a permanent silent failure.
        // One main-queue retry, no polling and no metric calls faking "used".
        dispatch_async(dispatch_get_main_queue(), ^{
            BOOL ready = picasso_install_statusbar();
            picasso_ack(ready ? "kern.picasso_metrics_installed" : "kern.picasso_metrics_unavailable");
        });
    }
}

__attribute__((constructor))
static void picasso_springboard_probe(void)
{
    // Record entry before either installation path can fail.
    picasso_ack("kern.picasso_inject_ack");
    @autoreleasepool {
        picasso_statusbar_start();
        picasso_install_astc_guard();
    }
}
