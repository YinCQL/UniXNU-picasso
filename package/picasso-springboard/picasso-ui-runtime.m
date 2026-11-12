// SPDX-License-Identifier: BSD-2-Clause-Patent
// Keep the independently verified seed source intact. This signed runtime
// build includes its status-bar/ASTC behavior plus the input cache and power-button corrections.
#include "picasso-springboard-probe.m"
#include "picasso-input-cache.m"
#include "picasso-power-button.m"

__attribute__((constructor))
static void picasso_input_start(void)
{
    _dyld_register_func_for_add_image(picasso_input_image_added);
    _dyld_register_func_for_add_image(picasso_power_image_added);
}
