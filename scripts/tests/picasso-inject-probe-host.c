// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#define OP_OPEN 2
#define MAXCOMLEN 16
typedef struct task_process *proc_t;
struct nameidata { int ni_op; };
static int selected;
static unsigned reports, name_reports;
static const char *process_name = "SpringBoard";
struct task_process { const char *p_name; };
static struct task_process test_process;
static proc_t current_proc(void) { test_process.p_name = process_name; return &test_process; }
bool picasso_dyld_inject_target(proc_t p) { return p == current_proc() && selected; }
static int proc_getpid(proc_t p) { assert(p == current_proc()); return 26; }
static void proc_name(int pid, char *name, size_t size) { assert(pid == 26); snprintf(name, size, "SpringBoard"); }
static void capture(const char *format, ...)
{
    va_list ap; va_start(ap, format);
    if (strstr(format, "PICASSO_BT_NAME")) {
        unsigned sample = va_arg(ap, unsigned), part = va_arg(ap, unsigned);
        for (unsigned i = 0; i < 4; ++i) assert(va_arg(ap, unsigned) <= 65535);
        assert(va_arg(ap, int) == 26 && sample <= 6 && part <= 3);
        assert(!strcmp(process_name, "Preferences")); ++name_reports;
    } else {
        unsigned ticket = va_arg(ap, unsigned); int pid = va_arg(ap, int);
        const char *name = va_arg(ap, const char *), *path = va_arg(ap, const char *);
        assert(strstr(format, "PICASSO_INJECT open=") && pid == 26);
        assert(!strcmp(name, "SpringBoard") && strstr(path, "picasso"));
        assert(ticket == ++reports);
    }
    va_end(ap);
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
    process_name = "Preferences";
    const char *prefix = "/private/var/picasso/picasso-marker-btname-";
    char marker[160];
    const char *invalid[] = {"1-1-0-1004-2-0-extra", "7-1-0-0-0-0", "1-4-0-0-0-0",
        "1-1-65536-0-0-0", "1-1-0-0-0", "1-1--1-0-0-0", "x-1-0-0-0-0"};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(*invalid); ++i) {
        snprintf(marker, sizeof(marker), "%s%s", prefix, invalid[i]);
        picasso_inject_open_probe(&nd, current_proc(), marker);
    }
    assert(name_reports == 0);
    snprintf(marker, sizeof(marker), "%s1-1-0-1004-2-0", prefix);
    selected = 0; picasso_inject_open_probe(&nd, current_proc(), marker); selected = 1;
    nd.ni_op = 0; picasso_inject_open_probe(&nd, current_proc(), marker); nd.ni_op = OP_OPEN;
    assert(name_reports == 0);
    for (unsigned i = 0; i < 10000; ++i) picasso_inject_open_probe(&nd, current_proc(), marker);
    assert(name_reports == 24 && reports == 64);
    puts("PASS: scoped read-only load/metric probes remain observable with PID1 tracing absent and saturate at 64");
}
