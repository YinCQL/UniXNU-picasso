// SPDX-License-Identifier: BSD-2-Clause-Patent
// PR13 board probes use the iOS16 display handoff as their late startup point.
// Keep the shared BootProgress UI disabled so it cannot replace iOS16 input.
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>

extern "C" {
void native_adreno620_set_embedded_firmware(void);
void native_adreno_probe(void);
void native_wlan_probe(void);
void picasso_adsp_poll(void);
void picasso_adsp_firmware_start(void);
void sheng_cpufreq_probe(void);
void sheng_cpufreq_poll(void);
void picasso_pr13_start(void);
// No WLAN firmware is supplied by PR13. Stage 3 reports its absence.
extern const uint8_t native_wlan_firmware[1] = {0};
extern const uint32_t native_wlan_firmware_size = 0;
volatile uint32_t native_legacy_gpu_fallbacks, native_legacy_gpu_last_status;
volatile uint64_t native_legacy_early_retires, native_legacy_early_failures;
void native_legacy_swap_stage(unsigned stage) { (void)stage; }
void native_legacy_gpu_fallback_observed(int status) {
    const uint32_t count=__atomic_add_fetch(&native_legacy_gpu_fallbacks, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&native_legacy_gpu_last_status, uint32_t(status), __ATOMIC_RELAXED);
    if(count<=8)IOLog("PICASSO_GPU copy failed count=%u status=%x; retry CPU\n",count,unsigned(status));
}
}

static void probes(void *, wait_result_t) {
    sheng_cpufreq_probe();
    native_adreno620_set_embedded_firmware();
    native_adreno_probe();
    native_wlan_probe();
    picasso_adsp_firmware_start();
    bool reported = false;
    for (;;) {
        sheng_cpufreq_poll();
        picasso_adsp_poll();
        const ml_topology_info_t *topology = ml_get_topology_info();
        if (!reported && topology) {
            const unsigned en = ml_get_cpu_number_type(CLUSTER_TYPE_E, true, true);
            const unsigned pn = ml_get_cpu_number_type(CLUSTER_TYPE_P, true, true);
            if (en + pn >= topology->num_cpus) {
                reported = true;
                IOLog("PICASSO_CPUFREQ topo online cpus=%u clusters=%u types=%x en=%u pn=%u cl-e=%u cl-p=%u\n",
                    topology->num_cpus, topology->num_clusters, topology->cluster_types, en, pn,
                    ml_get_cluster_number_type(CLUSTER_TYPE_E), ml_get_cluster_number_type(CLUSTER_TYPE_P));
            }
        }
        IOSleep(1000);
    }
}

extern "C" void picasso_pr13_start(void) {
    static uint32_t started;
    if (__atomic_exchange_n(&started, 1, __ATOMIC_ACQ_REL)) return;
    thread_t thread = nullptr;
    const kern_return_t status = kernel_thread_start(probes, nullptr, &thread);
    if (status == KERN_SUCCESS) thread_deallocate(thread);
    else {
        __atomic_store_n(&started, 0, __ATOMIC_RELEASE);
        IOLog("PICASSO_PR13 worker failed status=%x\n", status);
    }
}
