/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <infiltratr/filesystem_module_v2.h>
#include <stdio.h>
extern const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void);
int main(void)
{
    const IfsFsModuleApiV2 *api=infiltrator_filesystem_api_v2();
    if (!ifs_fs_api_v2_valid(api,"ext4") ||
        api->runtime_profiles != IFS_FS_PROFILE_HOSTED ||
        !ifs_fs_group_v2(api,IFS_FS_GROUP_CORE)) {
        fputs("EXT4.so does not expose the required Common V2 descriptor\n", stderr);
        return 1;
    }
    return 0;
}
