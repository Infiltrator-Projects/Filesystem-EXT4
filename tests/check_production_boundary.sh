#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# The shipped SO cannot retain the retired V1 filesystem / Defragmenter ABI.
set -eu
module=${1:?EXT4.so path required}
test -f "$module"
sh "$(dirname "$0")/check_v2_exports.sh" "$module"
if nm -D --defined-only "$module" | grep -E     'infiltrator_filesystem_ext_v1|ifs_ext_(worker_main|metadata_main)|ifs_bind_services|ifs_unbind_services|ifs_ext_fixture_run|ldtm_' ; then
    echo 'Production EXT4.so still exports retired Defragmenter workers or adapters' >&2
    exit 1
fi
