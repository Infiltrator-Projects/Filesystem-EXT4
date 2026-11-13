# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(Ext4BootReader LANGUAGES C)

# Build the firmware profile independently of the existing hosted package.
# Both use the Common V2 contract and the EXT4 owner's canonical format core.
set(COMMON_INCLUDE_DIR "" CACHE PATH "Include directory of Infiltrator Common")
if(NOT EXISTS "${COMMON_INCLUDE_DIR}/infiltratr/filesystem_module_v2.h")
    message(FATAL_ERROR "Set COMMON_INCLUDE_DIR to the pinned Infiltrator Common include directory")
endif()
add_library(ext4-boot MODULE ext4.c ../../native/filesystems/ext4/core/ext4_core.c)
set_target_properties(ext4-boot PROPERTIES PREFIX "" OUTPUT_NAME EXT4 C_VISIBILITY_PRESET hidden)
target_compile_features(ext4-boot PRIVATE c_std_11)
target_include_directories(ext4-boot PRIVATE "${COMMON_INCLUDE_DIR}")
target_compile_options(ext4-boot PRIVATE
    -Wall -Wextra -Wpedantic -Werror -ffreestanding -fno-builtin
    -fno-tree-loop-distribute-patterns -fno-stack-protector -mno-red-zone
    -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
target_link_options(ext4-boot PRIVATE
    -nostdlib -Wl,-Bsymbolic,-z,defs,-z,noexecstack,-z,relro,-z,now)
