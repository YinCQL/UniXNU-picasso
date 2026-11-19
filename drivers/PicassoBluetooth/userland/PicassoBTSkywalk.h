/* The private Skywalk memory ABI must match the built XNU, not the SDK. */
#include <stdio.h>
#ifndef __probable
#define __probable(x) __builtin_expect(!!(x), 1)
#endif
#ifndef __improbable
#define __improbable(x) __builtin_expect(!!(x), 0)
#endif
#define _mach_snprintf snprintf
#define fprintf_stderr picasso_sw_fprintf_stderr
