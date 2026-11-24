#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Independent on-disk validation of the actual Common V2 formatter output.
set -eu
testbin=${1:?formatter regression executable required}
checker=${2:?e2fsck required}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
"$testbin" "$work/single.img" 64 512
"$checker" -f -n "$work/single.img"
"$testbin" "$work/multiple.img" 140 4096
"$checker" -f -n "$work/multiple.img"
