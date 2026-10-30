/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ifs_ext_module_v1.h"
#include <stdio.h>
#include <string.h>
int main(void) {
    const IfsExtModuleV1 *module = infiltrator_filesystem_ext_v1();
    if (!module || module->abi_version != IFS_EXT_MODULE_V1 ||
        module->structure_bytes < sizeof(*module) ||
        strcmp(module->format_id, IFS_EXT_FORMAT_ID) != 0 ||
        module->capabilities != (IFS_EXT_CAP_RUN | IFS_EXT_CAP_METADATA | IFS_EXT_CAP_FIXTURE | IFS_EXT_CAP_PROBE) ||
        !module->probe || !module->run || !module->metadata || !module->fixture) {
        fputs("EXT.so module contract is incomplete\n", stderr);
        return 1;
    }
    return 0;
}
