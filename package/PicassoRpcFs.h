// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PICASSO_RPC_FS_H
#define PICASSO_RPC_FS_H
#include <stdint.h>
struct PicassoRpcArg;
struct PicassoRpcFsStats {
    uint32_t nodes,nodeLimit,tombstones,bytes,blocks,blockLimit,exhaustion;
    char target[128];
};
void picasso_rpc_fs_stats(PicassoRpcFsStats *stats);
// One boot-scoped namespace shared by the two listener PDs. No host filesystem
// access, no device calibration invented, and no persistent-write guarantee.
int picasso_rpc_fs_invoke(unsigned pd,uint32_t sc,PicassoRpcArg *args,char *name);
struct wlan_smem_io;
int picasso_rpc_fs_socinfo(const wlan_smem_io *io,uint32_t *values);
void picasso_rpc_fs_reset(); // host fixture/startup only, never while DMA is live
#endif
