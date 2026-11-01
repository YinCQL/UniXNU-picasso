// SPDX-License-Identifier: MIT
// Optional board probes referenced by the shared UniXNU boot monitor. Picasso
// does not implement the sheng touch/GPU telemetry, so these weak zero-valued
// definitions keep the monitor self-contained while allowing a real driver to
// override any of them later.
#include <stdint.h>

#define WEAK __attribute__((weak))

WEAK volatile uint32_t native_virtio_ready;
WEAK volatile uint32_t native_virtio_read_requests;
WEAK volatile uint32_t native_virtio_read_failures;
WEAK volatile uint64_t native_virtio_verified_blocks;

WEAK char native_touch_rows[4][120];
WEAK char native_cpufreq_row[120];
WEAK volatile uint32_t native_hid_input_events[8];
WEAK volatile int native_hid_input_results[8];
// Owned offline HID source (ShengHidInput). The real definitions live in
// libShengHidInput.a and override these; the weak set keeps the kernel link
// valid when CONFIG_XNU_SHENG_HIDINPUT is off.
WEAK void native_hid_input_probe(void) {}
WEAK void native_hid_swipe_tick(unsigned now) { (void)now; }
WEAK void native_novatek_touch_probe(void) {}
WEAK void native_novatek_touch_boot_report(unsigned now) { (void)now; }
WEAK volatile uint32_t native_hid_stage, native_hid_resource_found;
WEAK volatile int native_hid_load_status, native_hid_input_meta_status;
WEAK volatile uint64_t native_hid_input_object;
// Optional producers the sheng HID source binds weakly. The kernel links with
// -no_weak_imports, so the absent ones need real weak definitions here; the
// touch and button drivers replace them when they are ported.
// Shared HID digitizer sink. This file is linked in every configuration, so the
// storage has exactly one owner: the owned HID archive registers its notify
// callback here, and every producer (the synthetic swipe and the Novatek touch
// probe) calls native_touch_notify() after publishing a frame.
static void (*native_touch_sink_fn)(void *);
static void *native_touch_sink_context;
void native_touch_set_sink(void (*sink)(void *), void *context) {
    __atomic_store_n(&native_touch_sink_context, context, __ATOMIC_RELEASE);
    __atomic_store_n(&native_touch_sink_fn, sink, __ATOMIC_RELEASE);
}
void native_touch_notify(void) {
    void (*sink)(void *) = __atomic_load_n(&native_touch_sink_fn, __ATOMIC_ACQUIRE);
    if (sink) sink(__atomic_load_n(&native_touch_sink_context, __ATOMIC_ACQUIRE));
}
int native_touch_sink_ready(void) {
    return __atomic_load_n(&native_touch_sink_fn, __ATOMIC_ACQUIRE) != 0;
}
WEAK void native_buttons_set_sink(void (*sink)(void *, uint32_t), void *context) { (void)sink; (void)context; }
WEAK void (*sheng_ioconnect_hook)(void *, uint32_t, uint64_t, uint64_t, uint64_t);
WEAK volatile uint32_t native_touch_loop_step;
WEAK volatile uint32_t native_touch_seq;
volatile uint32_t native_touch_writer_lock;
WEAK volatile uint32_t native_touch_count;
// Referenced by the owned HID source (ShengHidInput) for its touch timeline.
WEAK volatile uint32_t native_touch_hid_reports;
WEAK volatile int native_touch_xs[12];
WEAK volatile int native_touch_ys[12];
WEAK volatile int native_touch_ids[12];
WEAK volatile uint32_t native_cpufreq_stage;
WEAK volatile unsigned native_touch_stage_req;

WEAK volatile uint32_t native_legacy_stage;
WEAK volatile uint32_t native_legacy_pm_stage;
WEAK volatile uint32_t native_legacy_pm_calls;
WEAK volatile uint32_t native_legacy_pm_callbacks;
WEAK volatile uint32_t native_legacy_swap_validation_calls;
WEAK volatile uint32_t native_legacy_swap_validation_result;
WEAK volatile uint32_t native_legacy_swap_validation_reason;
WEAK volatile uint32_t native_legacy_swap_accepted_count;
WEAK volatile uint32_t native_legacy_swap_rejected_count;
WEAK volatile uint64_t native_legacy_rej_surface[7];
WEAK volatile uint32_t native_legacy_rej_reason;
WEAK volatile uint64_t native_legacy_rej_surfaces[4];
WEAK volatile uint32_t native_legacy_rej_state[3];
WEAK volatile uint32_t native_legacy_rej_options[3];
WEAK volatile uint32_t native_legacy_rej_size[2];
WEAK volatile uint32_t native_legacy_rej_rects[2][4];
WEAK char native_legacy_rej_class[32];
WEAK volatile uint32_t native_legacy_frame_count;
WEAK volatile uint32_t native_legacy_frame_kind[32];
WEAK volatile uint32_t native_legacy_frame_time[32];
WEAK volatile uint64_t native_legacy_frame_stats[32][5];
WEAK volatile uint32_t native_legacy_frame_colors[8][4];
WEAK volatile uint64_t native_legacy_frame_color_counts[8][4];
WEAK volatile uint64_t native_legacy_frame_color_other[8];
WEAK volatile uint32_t native_legacy_transfer_calls;
WEAK volatile uint32_t native_legacy_retirement_calls;
WEAK volatile uint32_t native_legacy_guard_slot;
WEAK volatile uint32_t native_legacy_prepare_calls;
WEAK volatile uint32_t native_legacy_prepare_reason;
WEAK volatile uint32_t native_legacy_prepare_attempts;
WEAK volatile uint32_t native_legacy_client_calls;
WEAK volatile uint32_t native_legacy_client_type;
WEAK volatile int native_legacy_client_pid;
WEAK volatile int native_legacy_client_status;
WEAK char native_legacy_client_class[64];
WEAK volatile uint64_t native_legacy_gop_virtual;
WEAK volatile uint64_t native_legacy_gop_physical;
WEAK volatile uint64_t native_legacy_gop_bytes;
WEAK volatile int native_legacy_transfer_status;
WEAK volatile uint64_t native_legacy_transfer_bytes;
WEAK volatile uint64_t native_legacy_copy_n;
WEAK volatile uint64_t native_legacy_copy_last;
WEAK volatile uint64_t native_legacy_copy_max;
WEAK volatile uint64_t native_legacy_copy_sum;
WEAK volatile uint64_t native_legacy_copy_bytes_last;
WEAK volatile uint64_t native_legacy_copy_bytes_sum;
WEAK volatile uint64_t native_legacy_early_retires;
WEAK volatile uint64_t native_legacy_early_failures;
WEAK volatile uint32_t native_legacy_gpu_fallbacks;
WEAK volatile uint32_t native_legacy_gpu_last_status;

WEAK volatile uint64_t native_gop_gpu_offers;
WEAK volatile uint64_t native_gop_gpu_completions;
WEAK volatile uint64_t native_gop_gpu_failures;
WEAK volatile uint64_t native_gop_gpu_abandoned;

WEAK volatile uint32_t native_adreno_audit_attr;
WEAK volatile uint64_t native_adreno_audit_ticks;

WEAK void boot_log_tick(unsigned now) { (void)now; }
WEAK void sheng_cpufreq_poll(void) {}

WEAK uint64_t boot_diag_scan(uint64_t value) {
    (void)value;
    return 0;
}

WEAK int boot_diag_stream(int (*write)(const void *, uint32_t, void *), void *context) {
    (void)write;
    (void)context;
    return 0;
}
