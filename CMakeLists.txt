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
 GIT_TAG c76ec95ef66fc728df6a906ad75a6a4dd63b419b)
FetchContent_MakeAvailable(infiltratr_common)
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
 fixtures/ext_codec.c module/ext_fixture_bridge.c)
target_include_directories(ext-core PRIVATE "${EXT_SRC}" "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/include" "${CMAKE_CURRENT_SOURCE_DIR}/compat"
 "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(ext-core PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
target_compile_options(ext-core PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(ext-core PUBLIC InfiltratrCommon::Common OpenSSL::Crypto SQLite::SQLite3 Threads::Threads)
foreach(kind IN ITEMS ext2 ext3 ext4)
 string(TOUPPER "${kind}" module_name)
 add_library(ext-${kind}-driver SHARED
  $<TARGET_OBJECTS:ext-core>
  compat/legacy_services.c compat/runtime_alloc.c module/ext_module.c)
 set_target_properties(ext-${kind}-driver PROPERTIES PREFIX "" OUTPUT_NAME "${module_name}" SUFFIX ".so")
 target_compile_definitions(ext-${kind}-driver PRIVATE
  _GNU_SOURCE _FILE_OFFSET_BITS=64 IFS_EXT_FORMAT_ID="${kind}")
 target_compile_options(ext-${kind}-driver PRIVATE -Wall -Wextra -Wpedantic -Werror)
 target_link_options(ext-${kind}-driver PRIVATE -Wl,-z,defs)
 target_include_directories(ext-${kind}-driver PRIVATE
  "${EXT_SRC}" "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/include" "${CMAKE_CURRENT_SOURCE_DIR}/compat"
  "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_BINARY_DIR}")
 target_link_libraries(ext-${kind}-driver PRIVATE InfiltratrCommon::Common OpenSSL::Crypto SQLite::SQLite3 Threads::Threads)
 if(BUILD_TESTING)
  add_executable(ext-${kind}-module-test tests/test_module.c)
  target_compile_definitions(ext-${kind}-module-test PRIVATE IFS_EXT_FORMAT_ID="${kind}")
  target_include_directories(ext-${kind}-module-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/compat")
  target_link_libraries(ext-${kind}-module-test PRIVATE ext-${kind}-driver)
  add_test(NAME ext-${kind}-module-contract COMMAND ext-${kind}-module-test)
 endif()
 install(TARGETS ext-${kind}-driver LIBRARY DESTINATION /System/Filesystems)
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
endif()
