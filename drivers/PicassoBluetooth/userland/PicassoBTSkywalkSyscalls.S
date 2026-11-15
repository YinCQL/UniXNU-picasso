/* ARM64 BSD syscall ABI, XNU libsyscall/custom/SYS.h. These numbers are
 * checked against the same prepared XNU syscalls.master during each build. */
.text
.macro CHANNEL_TRAP name, number
.globl _picasso_sw_\name
.private_extern _picasso_sw_\name
.p2align 2
_picasso_sw_\name:
    mov x16, #\number
    svc #0x80
    b.cc 1f
    b _picasso_sw_syscall_error
1:  ret
.endm
CHANNEL_TRAP __channel_open, 510
CHANNEL_TRAP __channel_get_info, 511
CHANNEL_TRAP __channel_sync, 512
CHANNEL_TRAP __channel_get_opt, 513
CHANNEL_TRAP __channel_set_opt, 514
