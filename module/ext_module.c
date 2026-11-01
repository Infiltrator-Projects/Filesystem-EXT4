/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ifs_ext_module_v1.h"
#include "ifs_ext_fixture_v1.h"
#include "ext_native.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
int ifs_bind_services(const IfsDefragServicesV1 *);
void ifs_unbind_services(void);
int ifs_ext_worker_main(int, char **);
int ifs_ext_metadata_main(int, char **);
/* Probe through the independent engine: Defragmenter never parses an
 * EXT superblock to decide which module owns the selected media. */
static int probe(const char *path, const IfsDefragServicesV1 *services) {
    if (!path || ifs_bind_services(services) != 0) return 1;
    ExtGeometry geometry;
    char *error = NULL;
    const int valid = ext_read_geometry(path, &geometry, &error) == 0 &&
                      strcmp(geometry.filesystem, IFS_EXT_FORMAT_ID) == 0;
    free(error);
    ifs_unbind_services();
    return valid ? 0 : 1;
}
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
int ifs_ext_fixture_run(const void *, const char *, int, const void *);
/* The compatibility engine still contains all three fixture codecs.
 * Never allow one module to create or verify another format's media.
 * This check happens before binding callbacks or performing any I/O. */
static int owns_fixture(const LdtmFilesystemSpec *spec) {
    return spec && spec->key &&
           strcmp(spec->key, IFS_EXT_FORMAT_ID) == 0 &&
           ((spec->creator == LDTM_CREATOR_EXT2 && strcmp(IFS_EXT_FORMAT_ID, "ext2") == 0) ||
            (spec->creator == LDTM_CREATOR_EXT3 && strcmp(IFS_EXT_FORMAT_ID, "ext3") == 0) ||
            (spec->creator == LDTM_CREATOR_EXT4 && strcmp(IFS_EXT_FORMAT_ID, "ext4") == 0));
}
static int fixture(const void *spec, const char *path, int create,
                   const IfsDefragServicesV1 *services,
                   const IfsExtFixtureHostV1 *host) {
    if (!owns_fixture((const LdtmFilesystemSpec *)spec) || !path || !host) {
        errno = EINVAL;
        return -1;
    }
    if (ifs_bind_services(services) != 0) return -1;
    int result = ifs_ext_fixture_run(spec, path, create, host);
    ifs_unbind_services();
    return result;
}
static const IfsExtModuleV1 module = {
    IFS_EXT_MODULE_V1, sizeof(IfsExtModuleV1),
    IFS_EXT_FORMAT_ID, IFS_EXT_CAP_RUN | IFS_EXT_CAP_METADATA | IFS_EXT_CAP_FIXTURE | IFS_EXT_CAP_PROBE,
    probe, run, metadata, fixture
};
const IfsExtModuleV1 *infiltrator_filesystem_ext_v1(void) { return &module; }
