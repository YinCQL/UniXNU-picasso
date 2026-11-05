// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define SYSCTL_HANDLER_ARGS (int arg2, void *req)
#define os_atomic_load(p, order) (*(p))
#define os_atomic_cmpxchg(p, old, value, order) ((*(p) == (old)) ? (*(p) = (value), 1) : 0)
static int allowed = 1, printed;
static int current_proc(void) { return 1; }
static int picasso_dyld_inject_target(int p) { return p && allowed; }
static int proc_selfpid(void) { return 26; }
static void proc_name(int pid, char *name, size_t length)
{ assert(pid == 26); snprintf(name, length, "%s", "SpringBoard"); }
static int status_out(void *req, const void *p, size_t n)
{ assert(n == sizeof(int)); memcpy(req, p, n); return 0; }
#define SYSCTL_OUT status_out
static int status_printf(const char *format, ...)
{ assert(strstr(format, "PICASSO_ASTC_GUARD")); ++printed; return 0; }
#define printf status_printf
#include "astc-status.inc"
#undef printf
int main(void)
{
    int value = 0;
    assert(picasso_astc_status_sysctl(-1, &value) == EPERM);
    assert(picasso_astc_status_sysctl(6, &value) == EPERM);
    allowed = 0;
    assert(picasso_astc_status_sysctl(0, &value) == EPERM);
    assert(printed == 0 && value == 0);
    allowed = 1;
    for (int reason = 0; reason < 6; ++reason) {
        for (int i = 0; i < 40; ++i) assert(picasso_astc_status_sysctl(reason, &value) == 0);
        assert(printed == (reason + 1) * 12 && value == 1);
    }
    puts("PASS: ASTC observations require existing UI scope and stop logging at 12 per reason");
    return 0;
}
