/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int GLint;
#define GL_VIEWPORT 0x0ba2
struct eagl { unsigned frames, program, bound[8], colour, framebuffer; };
static unsigned function_given[90], text_calls, get_calls;
static float function_values[90][32];
static char captured[200];
static void nativegl_text(const char *text) {
    assert(strlen(text) < sizeof(captured));
    strcpy(captured, text);
    text_calls++;
}
static void glGetIntegerv(unsigned name, GLint *out) {
    assert(name == GL_VIEWPORT);
    GLint view[] = {3, 7, 1080, 2400};
    memcpy(out, view, sizeof(view));
    get_calls++;
}
#include "diagnostics.inc"
static void reset(void) {
    memset(compositor_keys, 0, sizeof(compositor_keys));
    memset(compositor_counts, 0, sizeof(compositor_counts));
    compositor_lock = text_calls = get_calls = 0;
}
static void *claim_many(void *arg) {
    (void)arg;
    for (unsigned i = 0; i < 100000; i++) compositor_claim(0, i % 40);
    return NULL;
}
int main(void) {
    const unsigned limits[] = {32, 16, 16, 8, 8, 16};
    reset();
    assert(!compositor_claim(6, 1) && !compositor_claim(UINT32_MAX, 1));
    compositor_lock = 1;
    assert(!compositor_claim(0, 1));
    assert(compositor_counts[0] == 0 && compositor_lock == 1);
    compositor_lock = 0;
    for (unsigned b = 0; b < 6; b++) {
        for (unsigned i = 0; i < limits[b]; i++) {
            assert(compositor_claim(b, i));
            assert(!compositor_claim(b, i));
        }
        for (unsigned i = limits[b]; i < 10000; i++) assert(!compositor_claim(b, i));
        assert(compositor_counts[b] == limits[b]);
    }
    reset();
    for (unsigned i = 0; i < 100; i++) compositor_program(i, 1, "test");
    assert(text_calls == 8 && compositor_counts[2] == 0);
    assert(compositor_claim(2, 1)); /* Program successes cannot exhaust error diagnostics. */
    char long_text[1000];
    memset(long_text, 'x', sizeof(long_text));
    long_text[sizeof(long_text) - 1] = 0;
    compositor_text("%s", long_text);
    assert(strlen(captured) == 199 && !strncmp(captured, "eagl comp ", 10));
    compositor_text("a\nb\rc\td");
    assert(!strcmp(captured, "eagl comp a b c d"));
    reset();
    struct eagl e = {.frames=4000, .program=12, .bound={4,8}, .colour=9, .framebuffer=3};
    struct eagl before = e;
    function_given[45] = 20;
    function_values[45][0] = 1.25f;
    function_values[45][19] = 0.5f;
    compositor_draw(&e, 45, 3);
    assert(get_calls == 1 && text_calls == 1);
    assert(strstr(captured, "view=3,7,1080,2400 n=20 v=1.25,0.5"));
    for (unsigned i = 0; i < 100000; i++) compositor_draw(&e, 45, 3);
    assert(get_calls == 1 && text_calls == 1 && !memcmp(&e, &before, sizeof(e)));
    compositor_draw(&e, 90, 0); /* Unknown function must not read function_values[90]. */
    assert(strstr(captured, "n=0 v=0,0"));
    compositor_draw(&e, UINT32_MAX, 0);
    compositor_draw(&e, 1, UINT32_MAX);
    assert(get_calls == 2);
    reset();
    for (unsigned i = 0; i < 90; i++) compositor_draw(&e, i, 9);
    assert(text_calls == 16);
    compositor_draw(&e, 45, 0);
    assert(text_calls == 17); /* Ordinary programs cannot exhaust fallback observations. */
    reset();
    pthread_t threads[8];
    for (unsigned i = 0; i < 8; i++) assert(!pthread_create(&threads[i], NULL, claim_many, NULL));
    for (unsigned i = 0; i < 8; i++) assert(!pthread_join(threads[i], NULL));
    assert(compositor_counts[0] == 32 && compositor_lock == 0);
    for (unsigned i = 0; i < 32; i++) for (unsigned j = 0; j < i; j++)
        assert(compositor_keys[0][i] != compositor_keys[0][j]);
    puts("PASS: independent budgets, deduplication, contention, bounded text, and read-only draw observations");
    return 0;
}
