// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include "picasso-astc-guard-core.h"

static unsigned char plugin[0x250], texture[0xe0];
static const uintptr_t vtable = 0x12345678;
static void pointer(void *object, unsigned offset, uintptr_t value)
{ memcpy((unsigned char *)object + offset, &value, sizeof(value)); }
static void number(void *object, unsigned offset, uint32_t value)
{ memcpy((unsigned char *)object + offset, &value, sizeof(value)); }
static void reset(void)
{
    memset(plugin, 0, sizeof(plugin)); memset(texture, 0, sizeof(texture));
    pointer(plugin, 0x1c0, (uintptr_t)texture); pointer(texture, 0, vtable);
    texture[0x29] = 1;
    number(texture, 0x1c, 4); number(texture, 0x20, 4);
    number(texture, 0x64, 1080); number(texture, 0x68, 2400);
}

int main(void)
{
    struct picasso_astc_request r = {0};
    reset();
    assert(picasso_astc_request(plugin, 0, vtable, &r) == PICASSO_ASTC_LAYOUT);
    assert(r.linear_bytes == 2592000 && r.format == 204);
    assert(!picasso_astc_layout_fits(&r, 0));
    assert(!picasso_astc_layout_fits(&r, 16));
    assert(!picasso_astc_layout_fits(&r, 1048576));
    assert(!picasso_astc_layout_fits(&r, 2591999));
    assert(picasso_astc_layout_fits(&r, 2592000));
    assert(picasso_astc_layout_fits(&r, 4194304));
    assert(!picasso_astc_layout_fits(&r, UINT64_C(1) << 32));
    assert(!picasso_astc_layout_fits(&r, (UINT64_C(1) << 32) + 2592000));
    number(texture, 0x1c, 8); number(texture, 0x20, 8);
    assert(picasso_astc_request(plugin, 0, vtable, &r) == PICASSO_ASTC_LAYOUT);
    assert(r.linear_bytes == 648000 && r.format == 212);
    number(texture, 0x64, 9); number(texture, 0x68, 17);
    assert(picasso_astc_request(plugin, 0, vtable, &r) == PICASSO_ASTC_LAYOUT);
    assert(r.linear_bytes == 96);
    number(texture, 0x1c, 4);
    assert(picasso_astc_request(plugin, 0, vtable, &r) == PICASSO_ASTC_INVALID);
    number(texture, 0x1c, 6); number(texture, 0x20, 6);
    assert(picasso_astc_request(plugin, 0, vtable, &r) == PICASSO_ASTC_INVALID);
    reset(); number(texture, 0x64, 0);
    assert(picasso_astc_request(plugin, 0, vtable, &r) == PICASSO_ASTC_INVALID);
    reset(); number(texture, 0x68, 0);
    assert(picasso_astc_request(plugin, 0, vtable, &r) == PICASSO_ASTC_INVALID);
    reset(); number(texture, 0x64, UINT32_MAX);
    assert(picasso_astc_request(plugin, 0, vtable, &r) == PICASSO_ASTC_INVALID);
    reset(); number(texture, 0x68, UINT32_MAX);
    assert(picasso_astc_request(plugin, 0, vtable, &r) == PICASSO_ASTC_INVALID);
    reset(); texture[0x29] = 0;
    assert(picasso_astc_request(plugin, 0, vtable, &r) == PICASSO_ASTC_PASS);
    reset(); texture[0x28] = 1;
    assert(picasso_astc_request(plugin, 0, vtable, &r) == PICASSO_ASTC_PASS);
    reset(); pointer(texture, 0, 0);
    assert(picasso_astc_request(plugin, 0, vtable, &r) == PICASSO_ASTC_INVALID);
    reset(); pointer(plugin, 0x1c0, 0);
    assert(picasso_astc_request(plugin, 0, vtable, &r) == PICASSO_ASTC_INVALID);
    assert(picasso_astc_request(NULL, 0, vtable, &r) == PICASSO_ASTC_INVALID);
    /* An unrelated ATX format need not have an ASTCTextureImp at all. */
    assert(picasso_astc_request(plugin, 1, vtable, &r) == PICASSO_ASTC_PASS);
    pointer(plugin, 0x228, 1);
    assert(picasso_astc_request(plugin, 1, vtable, &r) == PICASSO_ASTC_INVALID);
    pointer(plugin, 0x230, 1);
    assert(picasso_astc_request(plugin, 1, vtable, &r) == PICASSO_ASTC_PASS);
    pointer(plugin, 0x230, 0); pointer(plugin, 0x248, 1);
    assert(picasso_astc_request(plugin, 1, vtable, &r) == PICASSO_ASTC_PASS);
    reset(); pointer(plugin, 0x228, 1);
    assert(picasso_astc_request(plugin, 1, vtable, &r) == PICASSO_ASTC_LAYOUT);
    puts("PASS: ASTC/ATX routing, rounded block storage, zero/small/overflow layouts and private ABI refusal");
    return 0;
}
