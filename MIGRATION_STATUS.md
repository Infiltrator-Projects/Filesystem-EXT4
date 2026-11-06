# EXT4 local source consolidation — 6 November 2026

Every one of the 40 tracked files in the central Filesystems EXT4 native source tree at `a253fb724ce1fd760a361219281effb6abd71367` is present locally in this independent repository. Its 3 existing canonical core/design files were byte-identical, so preserved; the complete Linux VFS and embedded journal source was imported verbatim with author/license notices unchanged.

The independent EXT4 shared-object implementation and its qualification tests remain in this Git. The `standalone/kernel.json` builds `ext4.ko` from this repository's local `native/filesystems/ext4/` tree, not a pinned download of central Filesystems. Only shared Libraries/Common code remains an external source dependency for the native `.so` build.

Kernel build/runtime, journal recovery, media and root/EFI qualification remain separate acceptance gates. Do not delete the central EXT4 implementation until the legacy Filesystems CMake, CI and packaging consumers have switched and pass independent post-change checks.
