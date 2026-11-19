/* bluetoothd-only XNU Skywalk entry points. Shared regions retain all native
 * schema, slot, redzone and packet validation in the matching XNU wrappers. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <skywalk/os_skywalk_private.h>

_Static_assert(CSM_CURRENT_VERSION == 18, "Review the Bluetooth Skywalk ABI");

int picasso_sw_fprintf_stderr(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vfprintf(stderr, format, args);
    va_end(args);
    return result;
}

/* BSD traps return errno with carry set. Keep libc's thread-local errno and
 * -1 convention, so the upstream wrappers keep their normal cleanup paths. */
int picasso_sw_syscall_error(int error)
{
    errno = error;
    return -1;
}

void picasso_sw_install(intptr_t slide)
{
#include "skywalk-slots.inc"
}
