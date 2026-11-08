/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Verify that EXT4.so links and exposes the format-policy implementation
 * that its independent ext4.ko also compiles from native/filesystems/ext4/core.
 */
#include "ext4_core.h"
#include <stdio.h>

static int check(int condition, const char *case_name)
{
    if (!condition) {
        fprintf(stderr, "EXT4 canonical core failure: %s\n", case_name);
        return 1;
    }
    return 0;
}

int main(void)
{
    int errors = 0;

    errors += check(ifs_ext4_unsupported_incompat_features(0U) == 0U,
                    "empty feature set");
    errors += check(ifs_ext4_unsupported_incompat_features(0x80000000U) ==
                    0x80000000U, "unknown incompatible feature");
    errors += check(ifs_ext4_validate_inode_geometry(4096U, 256U, 11U) ==
                    IFS_EXT4_INODE_GEOMETRY_OK, "valid inode geometry");
    errors += check(ifs_ext4_validate_inode_geometry(4096U, 257U, 11U) ==
                    IFS_EXT4_INODE_GEOMETRY_INVALID_INODE_SIZE,
                    "reject non-power-of-two inode size");
    errors += check(ifs_ext4_validate_bigalloc(
                        IFS_EXT4_FEATURE_INCOMPAT_EXTENTS,
                        IFS_EXT4_FEATURE_RO_COMPAT_BIGALLOC, 0U) ==
                    IFS_EXT4_BIGALLOC_OK, "valid bigalloc features");
    errors += check(ifs_ext4_validate_bigalloc(
                        0U, IFS_EXT4_FEATURE_RO_COMPAT_BIGALLOC, 0U) ==
                    IFS_EXT4_BIGALLOC_REQUIRES_EXTENTS,
                    "reject bigalloc without extents");

    return errors ? 1 : 0;
}
