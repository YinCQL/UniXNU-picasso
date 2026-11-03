/* SPDX-License-Identifier: MIT */
/* Weak "not implemented" definitions for Adreno620Probe's integration hooks.
 *
 * A static kernel link cannot leave the optional hooks undefined (a plain weak_import is only resolvable in a dynamic
 * link), so each hook needs a definition. These return adreno620::ProbeHookAbsent; the probe treats that exactly like a
 * null import: it prints "hook absent", fails the stage and never fabricates success. A real implementation linked from
 * another force-loaded archive is a strong definition and overrides these weak ones. */
#include "Adreno620ProbeCore.h"
#include <stddef.h>
#include <stdint.h>

extern "C" {
int native_adreno620_gmu_boot(const uint8_t *, size_t, const uint8_t *, size_t) __attribute__((weak));
/* Strong definition comes from the generated embedded-firmware unit when the vendor blobs were staged at build time;
 * this weak one leaves the probe unfurnished. */
void native_adreno620_set_embedded_firmware(void) __attribute__((weak));
int native_adreno620_hfi_handshake(void) __attribute__((weak));
int native_adreno620_cp_start(void) __attribute__((weak));
int native_adreno620_cp_submit(uint32_t *) __attribute__((weak));
int native_adreno620_cp_wait(uint32_t) __attribute__((weak));
int native_adreno620_draw(void) __attribute__((weak));

int native_adreno620_gmu_boot(const uint8_t *, size_t, const uint8_t *, size_t) {
    return adreno620::ProbeHookAbsent;
}
void native_adreno620_set_embedded_firmware(void) {
}
int native_adreno620_hfi_handshake(void) {
    return adreno620::ProbeHookAbsent;
}
int native_adreno620_cp_start(void) {
    return adreno620::ProbeHookAbsent;
}
int native_adreno620_cp_submit(uint32_t *) {
    return adreno620::ProbeHookAbsent;
}
int native_adreno620_cp_wait(uint32_t) {
    return adreno620::ProbeHookAbsent;
}
int native_adreno620_draw(void) {
    return adreno620::ProbeHookAbsent;
}
}
