/* Executes the actual cache policy after a complete Apps view, with I/O counters. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef void *proc_t;
typedef uintptr_t user_addr_t;
static int native_picasso_apps_stage = 5, native_picasso_apps_status;
volatile uint32_t native_data_persist_state;
static uint32_t rebuild = 1, restore;
static int drops, imports, restores, restoreResult;
static int PE_parse_boot_argn(const char *n, void *v, size_t z) {
    assert(z == 4); *(uint32_t *)v = !strcmp(n, "picasso-ls-rebuild") ? rebuild : restore; return 1;
}
static void picasso_m2v_probe(void) {}
static void picasso_m2v_import(proc_t p, user_addr_t s) {(void)p;(void)s;++imports;}
static int picasso_lsp_restore(proc_t p, user_addr_t s) {(void)p;(void)s;++restores;return restoreResult;}
static void native_picasso_ls_store_drop(proc_t p, user_addr_t s) {(void)p;(void)s;++drops;}
#include "startup-cache.inc"
int main(void) {
    unsigned cases=0;
    for (unsigned state=0; state<9; ++state) {
        native_data_persist_state=state;
        for (unsigned enabled=0; enabled<2; ++enabled)
        for (unsigned aux=0; aux<2; ++aux)
        for (unsigned good=0; good<5; ++good) {
            rebuild=enabled;restore=aux;restoreResult=1;drops=imports=restores=0;
            native_picasso_apps_stage=good==1?4:5;
            native_picasso_apps_status=good==2?5:0;
            unsigned found=good==3?0xffe:0xfff, resolved=good==4?0xffe:0xfff;
            picasso_apps_cache_prepare(NULL,0,found,resolved);
            int valid=enabled && good==0;
            assert(imports==(valid && state!=2));
            assert(restores==(valid && aux && state!=2));
            assert(drops==(valid && (!aux || state==2)));
            ++cases;
        }
    }
    native_data_persist_state=3;rebuild=restore=1;restoreResult=-1;
    native_picasso_apps_stage=5;native_picasso_apps_status=0;drops=0;
    picasso_apps_cache_prepare(NULL,0,0xfff,0xfff);assert(drops==1);++cases;
    printf("PASS startup cache: %u production policy cases\n",cases);
}
