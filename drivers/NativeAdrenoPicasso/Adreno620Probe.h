/* SPDX-License-Identifier: MIT */
#ifndef ADRENO620_PROBE_H
#define ADRENO620_PROBE_H
#include <stddef.h>
#include <stdint.h>

/* Kernel-facing surface of the picasso GPU probe. The boot monitor calls
 * native_adreno_probe(); it parses sheng-gpu=<n> and starts a bounded kernel
 * thread that advances the state machine in Adreno620ProbeCore.h. The globals
 * below are the diagnostic contract used by the boot monitor, in the same
 * style as native_novatek_touch_*.
 *
 * Firmware is never stored in this repository. An integrator stages the vendor
 * blobs in a ramdisk (or embeds them through its own archive) and calls
 * native_adreno_set_firmware() before native_adreno_probe(). The function copies
 * the pointer table, not the bytes; the buffers must stay alive for the life of
 * the probe. */
#ifdef __cplusplus
extern "C" {
#endif

struct native_adreno620_firmware {
    const uint8_t *gmu;
    size_t gmu_bytes;
    const uint8_t *sqe;
    size_t sqe_bytes;
    const uint8_t *zap_mdt;
    size_t zap_mdt_bytes;
    const uint8_t *zap_segment[3];   /* a620_zap.b00, .b01, .b02 */
    size_t zap_segment_bytes[3];
};

void native_adreno_set_firmware(const struct native_adreno620_firmware *firmware);

/* The SQE image the probe validated (the vendor file bytes, including its 4-byte header), for the CP hook.
 * Returns -1 when no firmware was staged. */
int native_adreno620_sqe_firmware(const uint8_t **bytes, size_t *length);

/* The zap image the probe validated: the .mdt container (ELF header, program headers and the hash
 * segment, exactly qcom_mdt_read_metadata's metadata) plus the three split payloads. Missing
 * entries are left NULL. Returns -1 when no zap was staged. */
int native_adreno620_zap_firmware(const uint8_t **mdt, size_t *mdt_bytes,
    const uint8_t *segments[3], size_t segment_bytes[3]);

/* Releases the GMU GPU_SET out-of-band request (HOST2GMU_INTR_SET bit 31) once the CP is running, the last
 * step of mainline hw_init after the CP ring has been initialized. No-op before the GMU has been booted. */
void native_adreno620_gmu_release_gpu_set(void);

/* Strong entry point; safe to call once from the boot monitor. Without
 * sheng-gpu=<1..5> it returns without doing anything. */
void native_adreno_probe(void);

extern volatile uint32_t native_adreno_stage_req;
extern volatile uint32_t native_adreno_stage;
extern volatile uint32_t native_adreno_step;
extern volatile int native_adreno_error;
extern volatile uint32_t native_adreno_identity[8];
extern volatile uint32_t native_adreno_identity_count;
extern volatile uint32_t native_adreno_gx_powered;
extern volatile uint32_t native_adreno_smmu_id[3];
extern volatile uint32_t native_adreno_smmu_banks;
extern volatile uint32_t native_adreno_smmu_sids;
extern volatile uint32_t native_adreno_fw_crc;
extern volatile uint32_t native_adreno_fw_bytes;
extern volatile uint32_t native_adreno_gmu_blocks;
extern volatile uint32_t native_adreno_gmu_bytes;
extern volatile uint32_t native_adreno_sqe_version;
extern volatile uint32_t native_adreno_sqe_security;
extern volatile uint32_t native_adreno_zap_segments;
extern volatile uint32_t native_adreno_fence;
extern volatile uint32_t native_adreno_ticks;
/* 0 running, 1 finished, 2 failed. */
extern volatile uint32_t native_adreno_done;

#ifdef __cplusplus
}
#endif

#endif /* ADRENO620_PROBE_H */
