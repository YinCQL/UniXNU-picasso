// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define THREAD_TAG_MAINTHREAD 1
#define FALSE 0
#define NSEC_PER_SEC 1000000000ULL
#define NSEC_PER_MSEC 1000000ULL
#define NSEC_PER_USEC 1000ULL
typedef uint64_t user_addr_t;
struct proc {const char *name;};
struct task {unsigned task_shared_region_slide; uint64_t mach_header_vm_address;};
typedef struct task *task_t;
struct thread {unsigned tag; uint64_t tid, syscalls_mach, syscalls_unix; struct proc *p; task_t task;};
typedef struct thread *thread_t;
typedef struct {uint64_t pc, lr, sp, fp; int is64;} arm_saved_state_t;
static uint64_t fake_time;
static uint64_t pairs[64][2];
static unsigned reads, logs, headers, fail_read;
static char output[16384];
static size_t output_at;
static unsigned thread_get_tag(thread_t t) {return t->tag;}
static uint64_t thread_tid(thread_t t) {return t->tid;}
static struct proc *get_bsdthreadtask_info(thread_t t) {return t->p;}
static const char *proc_name_address(struct proc *p) {return p->name;}
static int is_saved_state64(arm_saved_state_t *s) {return s->is64;}
static uint64_t get_saved_state_pc(arm_saved_state_t *s) {return s->pc;}
static uint64_t get_saved_state_lr(arm_saved_state_t *s) {return s->lr;}
static uint64_t get_saved_state_sp(arm_saved_state_t *s) {return s->sp;}
static uint64_t get_saved_state_fp(arm_saved_state_t *s) {return s->fp;}
static uint64_t mach_absolute_time(void) {return fake_time;}
static void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *ticks) {*ticks = ns;}
static void absolutetime_to_nanoseconds(uint64_t ticks, uint64_t *ns) {*ns = ticks;}
static task_t get_threadtask(thread_t t) {return t->task;}
static int task_pid(task_t t) {(void)t; return 26;}
static int copyin(user_addr_t addr, void *out, size_t size) {
    ++reads;
    if (fail_read) return 14;
    assert(size == 16 && addr >= 0x1000 && addr < 0x1400 && !(addr & 15));
    memcpy(out, pairs[(addr - 0x1000) / 16], size);
    return 0;
}
static int note(const char *format, ...) {
    char line[2048];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    assert(n > 0 && (size_t)n < sizeof(line));
    if (!strncmp(line, "PICASSO_UI_SAMPLE", 17)) ++headers;
    ++logs;
    if (output_at + (size_t)n < sizeof(output)) {
        memcpy(output + output_at, line, (size_t)n + 1);
        output_at += (size_t)n;
    }
    return n;
}
#define printf note
#include "ui-self-sample.inc"
#undef printf

static void clear_output(void) {reads = logs = headers = 0; output_at = 0; output[0] = 0;}
int main(void) {
    struct proc p = {"SpringBoard"};
    struct task task = {0x13984000, 0x10061a000};
    struct thread t = {0, 322, 100, 100, &p, &task};
    arm_saved_state_t s = {0x1d5f95854, 0x1a7ede638, 0x1000, 0x1000, 1};
    fake_time = 121 * NSEC_PER_SEC;
    picasso_ui_self_sample(&s, &t, 75);
    assert(!reads && !logs);
    t.tag = THREAD_TAG_MAINTHREAD; p.name = "Preferences";
    picasso_ui_self_sample(&s, &t, 75);
    assert(!reads && !logs);
    p.name = "SpringBoard"; s.is64 = 0;
    picasso_ui_self_sample(&s, &t, 75);
    assert(!reads && !logs);
    s.is64 = 1; fake_time = 119 * NSEC_PER_SEC;
    picasso_ui_self_sample(&s, &t, 75);
    assert(!reads && !logs);
    arm_saved_state_t original = s;
    pairs[0][0] = 0x1010; pairs[0][1] = 0x20000;
    pairs[1][0] = 0; pairs[1][1] = 0x30000;
    fake_time = 120 * NSEC_PER_SEC;
    picasso_ui_self_sample(&s, &t, 75);
    assert(headers == 1 && reads == 2 && logs == 2);
    assert(strstr(output, "depth=4 stop=0") && strstr(output, "20000 30000"));
    assert(!memcmp(&s, &original, sizeof(s)));
    clear_output();
    picasso_ui_self_sample(&s, &t, 75);
    assert(!reads && !logs);
    fake_time += 5 * NSEC_PER_SEC; t.tid = 400;
    picasso_ui_self_sample(&s, &t, 75);
    assert(!reads && !logs);
    t.tid = 322; s.fp = 0x1001;
    picasso_ui_self_sample(&s, &t, 75);
    assert(!reads && headers == 1 && strstr(output, "depth=2 stop=1"));
    clear_output(); fake_time += 5 * NSEC_PER_SEC; s.fp = 0x1000; fail_read = 1;
    picasso_ui_self_sample(&s, &t, 75);
    assert(reads == 1 && strstr(output, "depth=2 stop=2"));
    clear_output(); fake_time += 5 * NSEC_PER_SEC; fail_read = 0; pairs[0][0] = 0x1000;
    picasso_ui_self_sample(&s, &t, 75);
    assert(reads == 1 && strstr(output, "depth=3 stop=3") && strstr(output, "20000 0"));
    clear_output(); fake_time += 5 * NSEC_PER_SEC; s.fp = 0x7ffffffff0ULL;
    picasso_ui_self_sample(&s, &t, 75);
    assert(!reads && strstr(output, "depth=2 stop=1"));
    clear_output(); fake_time += 5 * NSEC_PER_SEC; s.fp = 0x1000;
    for (unsigned i = 0; i < 64; ++i) {pairs[i][0] = 0x1010 + i * 16; pairs[i][1] = 0x20000 + i * 4;}
    picasso_ui_self_sample(&s, &t, -47);
    assert(reads == 30 && headers == 1 && logs == 9 && strstr(output, "depth=32 stop=4"));
    s.fp = 0; clear_output();
    for (unsigned i = 6; i < 180; ++i) {
        fake_time += 5 * NSEC_PER_SEC;
        t.syscalls_mach += 10;
        picasso_ui_self_sample(&s, &t, -47);
    }
    assert(headers == 174 && !reads);
    clear_output(); fake_time += 5 * NSEC_PER_SEC;
    picasso_ui_self_sample(&s, &t, -47);
    assert(!reads && !logs);
    puts("PASS main-thread identity, rate/budget, bounded stack, bad pointers, read errors, cycles, and saved-state preservation");
}
