// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// libpicasso-sb.dylib (probe): a do-nothing library the kernel inserts into
// SpringBoard on this board (boot-arg picasso_inject_sb=PATH, see
// bsd/kern/kern_exec.c). Its only job is to prove the injection pipeline works
// end to end: the kernel added DYLD_INSERT_LIBRARIES, AMFI allowed an ad-hoc
// signed library into the process, and dyld mapped it and ran its constructor.
//
// It confirms it ran two ways. SpringBoard is sandboxed, so a /dev/console
// write may be dropped; the reliable signal is reading the kernel sysctl
// kern.picasso_inject_ack, whose handler prints the caller's pid to the
// console (a sysctl read the sandbox allows). No SpringBoard class, method or
// state is touched here; the device-specific logic that a real injected
// library would carry is NOT in this file.
#include <fcntl.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

__attribute__((constructor))
static void picasso_sb_probe(void)
{
    int value = 0;
    size_t size = sizeof(value);
    // The kernel handler prints "PICASSO_SBINJECT library ran ... pid=..." when this is read.
    sysctlbyname("kern.picasso_inject_ack", &value, &size, NULL, 0);

    static const char line[] = "PICASSO_SBINJECT probe library loaded into SpringBoard\n";
    int fd = open("/dev/console", O_WRONLY);
    if (fd >= 0) {
        (void)write(fd, line, strlen(line));
        close(fd);
    }
}
