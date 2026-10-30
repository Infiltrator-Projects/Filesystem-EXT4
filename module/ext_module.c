/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ifs_ext_module_v1.h"
int ifs_bind_services(const IfsDefragServicesV1 *);
void ifs_unbind_services(void);
int ifs_ext_worker_main(int, char **);
int ifs_ext_metadata_main(int, char **);
static int run(int argc, char **argv, const IfsDefragServicesV1 *services) {
    if (ifs_bind_services(services) != 0) return 1;
    int result = ifs_ext_worker_main(argc, argv);
    ifs_unbind_services();
    return result;
}
static int metadata(int argc, char **argv, const IfsDefragServicesV1 *services) {
    if (ifs_bind_services(services) != 0) return 1;
    int result = ifs_ext_metadata_main(argc, argv);
    ifs_unbind_services();
    return result;
}
static const IfsExtModuleV1 module = {
    IFS_EXT_MODULE_V1, sizeof(IfsExtModuleV1),
    IFS_EXT_FORMAT_ID, IFS_EXT_CAP_RUN | IFS_EXT_CAP_METADATA,
    run, metadata
};
const IfsExtModuleV1 *infiltrator_filesystem_ext_v1(void) { return &module; }
