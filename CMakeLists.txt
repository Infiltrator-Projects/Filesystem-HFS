# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" FILESYSTEM_VERSION LIMIT_COUNT 1)
project(FilesystemHFS VERSION ${FILESYSTEM_VERSION} LANGUAGES C)
include(CTest)
include(FetchContent)
find_package(OpenSSL REQUIRED)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(INFILTRATR_COMMON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(INFILTRATR_COMMON_BUILD_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_Declare(infiltratr_common
  GIT_REPOSITORY https://github.com/Infiltrator-Projects/Libraries.git
  GIT_TAG d23a52693fd3212368d327e1c1302e07ae9510ec)
FetchContent_MakeAvailable(infiltratr_common)
set_property(DIRECTORY "${infiltratr_common_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/version.h"
  "#define LD_VERSION \"${PROJECT_VERSION}\"\n")

# Production ships just Common V2 and its real hosted on-disk geometry reader.
# Historical workers and V1 host callbacks are not part of the runtime ELF.
add_library(hfs-driver SHARED module/hfs_module.c)
set_target_properties(hfs-driver PROPERTIES
  PREFIX "" OUTPUT_NAME "HFS" SUFFIX ".so"
  C_VISIBILITY_PRESET hidden)
target_compile_features(hfs-driver PRIVATE c_std_11)
target_compile_options(hfs-driver PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_options(hfs-driver PRIVATE
  -Wl,-z,defs
  "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/module/v2-only.map")
target_include_directories(hfs-driver PRIVATE
  "${infiltratr_common_SOURCE_DIR}/include")
target_link_libraries(hfs-driver PRIVATE InfiltratrCommon::Common)

if(BUILD_TESTING)
  # Source retained and compiled for regression; never installed.
  add_library(hfs-historical-test-support SHARED
    native/writer.c
    compat/legacy_services.c
    module/hfs_module.c
    fixtures/hfs_codec.c
    module/hfs_fixture_bridge.c)
  set_source_files_properties(fixtures/hfs_codec.c PROPERTIES COMPILE_DEFINITIONS "ldtm_raw_mac=ifs_hfs_raw_fixture")
  target_compile_definitions(hfs-historical-test-support PRIVATE
    _GNU_SOURCE _FILE_OFFSET_BITS=64)
  target_compile_options(hfs-historical-test-support PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  target_include_directories(hfs-historical-test-support PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/native"
    "${CMAKE_CURRENT_SOURCE_DIR}/compat"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/include"
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${infiltratr_common_SOURCE_DIR}/include")
  target_link_libraries(hfs-historical-test-support PRIVATE
    InfiltratrCommon::Common OpenSSL::Crypto)

  add_executable(hfs-module-test tests/test_module.c)
  target_include_directories(hfs-module-test PRIVATE
    "${infiltratr_common_SOURCE_DIR}/include")
  target_link_libraries(hfs-module-test PRIVATE
    hfs-driver InfiltratrCommon::Common)
  add_test(NAME hfs-common-v2-probe-mount
    COMMAND hfs-module-test)
  add_test(NAME hfs-production-only-v2
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_v2_exports.sh"
      "$<TARGET_FILE:hfs-driver>")
endif()
install(TARGETS hfs-driver LIBRARY DESTINATION /System/Filesystems)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/HFS.pdf"
  DESTINATION /System/Resources/Documentation
  RENAME Filesystem-HFS.pdf)
