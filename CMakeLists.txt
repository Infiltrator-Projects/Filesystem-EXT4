cmake_minimum_required(VERSION 3.20)
project(FilesystemEXT VERSION 1.0.0 LANGUAGES C)
include(CTest)
include(FetchContent)
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(INFILTRATR_COMMON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(INFILTRATR_COMMON_BUILD_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_Declare(infiltratr_common
 GIT_REPOSITORY https://github.com/Infiltrator-Projects/Libraries.git
 GIT_TAG 4c92c8dcd7e03afd4bab3105e143dbabd72c8adc)
FetchContent_MakeAvailable(infiltratr_common)
# The embedded Common SDK is linked, not installed with filesystem runtime modules.
set_property(DIRECTORY "${infiltratr_common_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
# Keep native PGO, LTO and CPU flags out of the firmware ELF. The installed
# shared object carries these portable bytes as a non-allocated ELF section.
set(EXT4_FIRMWARE_DIR "${CMAKE_CURRENT_BINARY_DIR}/firmware")
set(EXT4_FIRMWARE_ELF "${EXT4_FIRMWARE_DIR}/EXT4.so")
add_custom_command(OUTPUT "${EXT4_FIRMWARE_ELF}"
 COMMAND "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_SOURCE_DIR}/module/firmware"
  -B "${EXT4_FIRMWARE_DIR}" -DCMAKE_BUILD_TYPE=Release
  "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
  "-DCOMMON_INCLUDE_DIR=${infiltratr_common_SOURCE_DIR}/include"
 COMMAND "${CMAKE_COMMAND}" --build "${EXT4_FIRMWARE_DIR}"
 DEPENDS module/firmware/CMakeLists.txt module/firmware/ext4.c
  native/filesystems/ext4/core/ext4_core.c native/filesystems/ext4/core/ext4_core.h
  "${infiltratr_common_SOURCE_DIR}/include/infiltratr/filesystem_module_v2.h"
 VERBATIM)
add_custom_target(ext4-firmware-elf DEPENDS "${EXT4_FIRMWARE_ELF}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/version.h" "#define LD_VERSION \"1.0.0\"\n")
set(EXT_SRC "${CMAKE_CURRENT_SOURCE_DIR}/migration/defragmenter/native")
set_source_files_properties("${EXT_SRC}/ext_worker.c" PROPERTIES
 COMPILE_DEFINITIONS "main=ifs_ext_worker_main")
set_source_files_properties("${EXT_SRC}/ext_metadata_worker.c" PROPERTIES
 COMPILE_DEFINITIONS "main=ifs_ext_metadata_main")
add_library(ext-core OBJECT
 "${EXT_SRC}/ext_disk.c"
 "${EXT_SRC}/ext_common.c"
 "${EXT_SRC}/ext_catalog.c"
 "${EXT_SRC}/ext_plan.c"
 "${EXT_SRC}/ext_workspace.c"
 "${EXT_SRC}/ext_worker.c"
 "${EXT_SRC}/ext_metadata_worker.c"
 "${CMAKE_CURRENT_SOURCE_DIR}/native/filesystems/ext4/core/ext4_core.c"
 fixtures/ext_codec.c module/ext_fixture_bridge.c)
target_include_directories(ext-core PRIVATE "${EXT_SRC}" "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/include" "${CMAKE_CURRENT_SOURCE_DIR}/compat"
 "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(ext-core PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
target_compile_options(ext-core PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(ext-core PUBLIC InfiltratrCommon::Common OpenSSL::Crypto SQLite::SQLite3 Threads::Threads)
# EXT2 and EXT3 now have source-owned independent test builds.
# Never build those variants out of the EXT4 repository.
foreach(kind IN ITEMS ext4)
 string(TOUPPER "${kind}" module_name)
 add_library(ext-${kind}-driver SHARED
  $<TARGET_OBJECTS:ext-core>
  compat/legacy_services.c compat/runtime_alloc.c module/ext_module.c
  module/ext_v2_probe.c)
 set_target_properties(ext-${kind}-driver PROPERTIES PREFIX "" OUTPUT_NAME "${module_name}" SUFFIX ".so")
 target_compile_definitions(ext-${kind}-driver PRIVATE
  _GNU_SOURCE _FILE_OFFSET_BITS=64 IFS_EXT_FORMAT_ID="${kind}")
 target_compile_options(ext-${kind}-driver PRIVATE -Wall -Wextra -Wpedantic -Werror)
 target_link_options(ext-${kind}-driver PRIVATE -Wl,-z,defs)
 target_include_directories(ext-${kind}-driver PRIVATE
  "${EXT_SRC}" "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/include" "${CMAKE_CURRENT_SOURCE_DIR}/compat"
  "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_BINARY_DIR}"
  "${infiltratr_common_SOURCE_DIR}/include")
 target_link_libraries(ext-${kind}-driver PRIVATE InfiltratrCommon::Common OpenSSL::Crypto SQLite::SQLite3 Threads::Threads)
 add_dependencies(ext-${kind}-driver ext4-firmware-elf)
 set_property(TARGET ext-${kind}-driver APPEND PROPERTY LINK_DEPENDS "${EXT4_FIRMWARE_ELF}")
 add_custom_command(TARGET ext-${kind}-driver POST_BUILD
  COMMAND "${CMAKE_OBJCOPY}" --add-section ".infiltrator_boot=${EXT4_FIRMWARE_ELF}"
   --set-section-flags .infiltrator_boot=readonly "$<TARGET_FILE:ext-${kind}-driver>"
  VERBATIM)
 if(BUILD_TESTING)
  add_executable(ext-${kind}-module-test tests/test_module.c)
  target_compile_definitions(ext-${kind}-module-test PRIVATE IFS_EXT_FORMAT_ID="${kind}")
  target_include_directories(ext-${kind}-module-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/compat")
  target_link_libraries(ext-${kind}-module-test PRIVATE ext-${kind}-driver)
  add_test(NAME ext-${kind}-module-contract COMMAND ext-${kind}-module-test)
  add_executable(ext4-v2-probe-test tests/test_ext_v2_probe.c)
  target_include_directories(ext4-v2-probe-test PRIVATE "${infiltratr_common_SOURCE_DIR}/include")
  target_link_libraries(ext4-v2-probe-test PRIVATE ext-${kind}-driver InfiltratrCommon::Common)
  target_compile_options(ext4-v2-probe-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME ext4-common-v2-probe COMMAND ext4-v2-probe-test)
 endif()
 # EXT4 owns only this format's module and documentation.
 if(kind STREQUAL "ext4")
  install(TARGETS ext-${kind}-driver LIBRARY DESTINATION /System/Filesystems)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/EXT4.pdf"
          DESTINATION /System/Resources/Documentation RENAME Filesystem-EXT4.pdf)
 endif()
endforeach()

# The bitmap/descriptor cache regression came from Defragmenter, but the
# private EXT disk implementation and its native unit tests live here.
if(BUILD_TESTING)
 add_library(ext-test-host STATIC tests/test_host_services.c)
 target_include_directories(ext-test-host PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/compat")
 target_compile_definitions(ext-test-host PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
 target_compile_options(ext-test-host PRIVATE -Wall -Wextra -Wpedantic -Werror)
 target_link_libraries(ext-test-host PUBLIC ext-ext4-driver)
 add_executable(ext-bitmap-cache-test tests/test_ext_bitmap_cache.c)
 target_include_directories(ext-bitmap-cache-test PRIVATE
   "${CMAKE_CURRENT_SOURCE_DIR}/migration/defragmenter/native" "${CMAKE_CURRENT_SOURCE_DIR}/compat" "${CMAKE_CURRENT_SOURCE_DIR}/tests")
 target_compile_definitions(ext-bitmap-cache-test PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
 target_compile_options(ext-bitmap-cache-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
 target_link_libraries(ext-bitmap-cache-test PRIVATE ext-test-host InfiltratrCommon::Common OpenSSL::Crypto SQLite::SQLite3 Threads::Threads)
 add_test(NAME ext-bitmap-cache COMMAND ext-bitmap-cache-test)
 # The kernel adapter builds this same portable core from core_bridge.c;
 # demand its symbols and validation semantics in EXT4.so as well.
 add_executable(ext-core-policy-test tests/test_canonical_core.c)
 target_include_directories(ext-core-policy-test PRIVATE
   "${CMAKE_CURRENT_SOURCE_DIR}/native/filesystems/ext4/core")
 target_compile_options(ext-core-policy-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
 target_link_libraries(ext-core-policy-test PRIVATE ext-ext4-driver)
 add_test(NAME ext-canonical-core-policy COMMAND ext-core-policy-test)
endif()
