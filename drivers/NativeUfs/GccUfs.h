// SPDX-License-Identifier: BSD-2-Clause-Patent
// SM7250: gcc-lito.c and vendor/qcom/lito-gdsc.dtsi in the stock kernel.
// These resources are used only for passive inspection of firmware state.
#ifndef NATIVE_UFS_GCC_UFS_H
#define NATIVE_UFS_GCC_UFS_H
#define GCC_BASE                       0x00100000ULL
#define UFS_HC_BASE                    0x01d84000ULL
#define UFS_HC_SIZE                    0x3000ULL
#define GCC_UFS_PHY_GDSCR              0x77004
#define GCC_UFS_PHY_AXI_ENABLE         0x77010
#define GCC_UFS_PHY_AHB_ENABLE         0x77018
#define GCC_UFS_PHY_TX_SYMBOL0_ENABLE  0x7701c
#define GCC_UFS_PHY_RX_SYMBOL0_ENABLE  0x77020
#define GCC_UFS_PHY_UNIPRO_CORE_ENABLE 0x7705c
#define GCC_UFS_PHY_ICE_CORE_ENABLE    0x77064
#define GCC_UFS_PHY_PHY_AUX_ENABLE     0x7709c
#define GCC_UFS_PHY_RX_SYMBOL1_ENABLE  0x770b8
#define GCC_AGGRE_UFS_PHY_AXI_ENABLE   0x770cc
#define GCC_UFS_BRANCH_ENABLE_BIT      0x1U
// AXI and AHB have HWCG bit 1. CLK_OFF on an enabled, hardware-gated
// branch does not by itself mean that register access is unavailable.
#endif
