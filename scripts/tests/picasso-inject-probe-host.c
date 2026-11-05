// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define OP_OPEN 2
#define MAXCOMLEN 16
typedef void *proc_t;
struct nameidata { int ni_op; };
static int selected;
static unsigned reports;
static proc_t current_proc(void) { return (void *)1; }
bool picasso_dyld_inject_target(proc_t p) { return p == current_proc() && selected; }
static int proc_getpid(proc_t p) { assert(p == current_proc()); return 26; }
static void proc_name(int pid, char *name, size_t size) { assert(pid == 26); snprintf(name, size, "SpringBoard"); }
static void capture(const char *format, unsigned ticket, int pid, const char *name, const char *path)
{
    assert(strstr(format, "PICASSO_INJECT open=") && pid == 26);
    assert(!strcmp(name, "SpringBoard") && strstr(path, "picasso"));
    assert(ticket == ++reports);
}
#define printf capture
#include "inject-probe.inc"
#undef printf
int main(void)
{
    struct nameidata nd = {OP_OPEN};
    const char *library = "/private/var/picasso/libpicasso-sb.dylib";
    selected = 0; picasso_inject_open_probe(&nd, current_proc(), library);
    selected = 1; picasso_inject_open_probe(&nd, NULL, library);
    picasso_inject_open_probe(&nd, (void *)2, library);
    nd.ni_op = 0; picasso_inject_open_probe(&nd, current_proc(), library);
    nd.ni_op = OP_OPEN;
    picasso_inject_open_probe(&nd, current_proc(), "/private/var/picasso/other");
    picasso_inject_open_probe(&nd, current_proc(), "/private/var/picasso/libpicasso-sb.dylib.extra");
    assert(reports == 0);
    picasso_inject_open_probe(&nd, current_proc(), library);
    picasso_inject_open_probe(&nd, current_proc(), "/private/var/picasso/picasso-marker-inject_ack");
    assert(reports == 2);
    for (unsigned i = 0; i < 10000; ++i)
        picasso_inject_open_probe(&nd, current_proc(), "/private/var/picasso/picasso-marker-metrics_used");
    assert(reports == 64);
    puts("PASS: scoped read-only load/metric probes remain observable with PID1 tracing absent and saturate at 64");
}
