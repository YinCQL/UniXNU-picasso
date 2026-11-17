// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef ARM_CPU_SPI_H
#define ARM_CPU_SPI_H
#include <stdint.h>

// Raw readback retains group/enabled/edge at bits 0..2 and priority at 8..15.
// SecurityExtn==1 implies DS==0: IGROUPR is Secure and RAZ/WI to this
// Non-secure kernel. Mark that view explicitly; never synthesize its group bit.
static const uint32_t kArmCpuSpiGroupSecure = 1U << 16;
static inline uint32_t ArmCpuSpiReadbackView(uint32_t readback, uint32_t typer)
{
    return readback | ((typer & (1U << 10)) ? kArmCpuSpiGroupSecure : 0U);
}

// A Secure interrupt's ISENABLER bit is also RAZ/WI to Non-secure accesses,
// so a readable enabled bit qualifies access even when IGROUPR is hidden.
// This verifies configuration only; actual delivery still needs IRQ evidence.
static inline bool ArmCpuSpiLevelConfigured(uint32_t readback)
{
    return (readback & 6U) == 2U && ((readback >> 8) & 0xffU) == 0xa0U &&
           ((readback & 1U) || (readback & kArmCpuSpiGroupSecure));
}
#endif
