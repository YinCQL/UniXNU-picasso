// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef void *proc_t;
typedef const char *vnode_t;
static unsigned names, releases, scope_calls;
static bool selected;
static proc_t current_proc(void){return (void *)1;}
static const char *vnode_getname(vnode_t v){++names;return v;}
static void vnode_putname(const char *v){assert(v);++releases;}
static bool picasso_dyld_inject_target(proc_t p){++scope_calls;return p==current_proc() && selected;}
#include "inject-load-ticket.inc"
int main(void) {
    assert(!picasso_inject_load_ticket(NULL,"libpicasso-sb.dylib"));
    assert(!picasso_inject_load_ticket(current_proc(),NULL));
    assert(!names && !scope_calls);
    assert(!picasso_inject_load_ticket(current_proc(),"other.dylib"));
    assert(!picasso_inject_load_ticket(current_proc(),"libpicasso-sb.dylib.extra"));
    assert(names==2 && releases==2 && !scope_calls);
    assert(!picasso_inject_load_ticket(current_proc(),"libpicasso-sb.dylib"));
    selected=true;
    for(unsigned i=0;i<110;++i)
        assert(picasso_inject_load_ticket(current_proc(),"libpicasso-sb.dylib")== (i<96?i+1:0));
    assert(names==releases);
    puts("PASS: load-result ticket filters scope/basename, balances vnode names, saturates without wrap");
}
