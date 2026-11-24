# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" FILESYSTEM_VERSION LIMIT_COUNT 1)
project(FilesystemHFS VERSION ${FILESYSTEM_VERSION} LANGUAGES C)
include(CTest)
include(FetchContent)
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
add_library(hfs-driver SHARED module/hfs_module.c native/hfs_catalog_v2.c)
set_target_properties(hfs-driver PROPERTIES
  PREFIX "" OUTPUT_NAME "HFS" SUFFIX ".so"
  C_VISIBILITY_PRESET hidden)
target_compile_features(hfs-driver PRIVATE c_std_11)
target_compile_options(hfs-driver PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_options(hfs-driver PRIVATE
  -Wl,-z,defs
  "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/module/v2-only.map")
target_include_directories(hfs-driver PRIVATE
  "${infiltratr_common_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/native")
target_link_libraries(hfs-driver PRIVATE InfiltratrCommon::Common)

# The freestanding HFS reader contains the identical native catalogue code.
# Its complete no-import ELF is embedded in the installed hosted shared object.
set(HFS_FIRMWARE_DIR "${CMAKE_CURRENT_BINARY_DIR}/firmware")
set(HFS_FIRMWARE_ELF "${HFS_FIRMWARE_DIR}/HFS.so")
add_custom_command(OUTPUT "${HFS_FIRMWARE_ELF}"
  COMMAND "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_SOURCE_DIR}/module/firmware"
    -B "${HFS_FIRMWARE_DIR}" -DCMAKE_BUILD_TYPE=Release
    "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
    "-DCOMMON_INCLUDE_DIR=${infiltratr_common_SOURCE_DIR}/include"
  COMMAND "${CMAKE_COMMAND}" --build "${HFS_FIRMWARE_DIR}" --parallel 2
  DEPENDS module/firmware/CMakeLists.txt module/firmware/runtime.c
    native/hfs_catalog_v2.c native/hfs_catalog_v2.h module/hfs_module.c
    "${infiltratr_common_SOURCE_DIR}/include/infiltratr/filesystem_module_v2.h"
  VERBATIM)
add_custom_target(hfs-firmware-elf DEPENDS "${HFS_FIRMWARE_ELF}")
if(NOT CMAKE_OBJCOPY)
  find_program(CMAKE_OBJCOPY NAMES objcopy llvm-objcopy REQUIRED)
endif()
add_dependencies(hfs-driver hfs-firmware-elf)
set_property(TARGET hfs-driver APPEND PROPERTY LINK_DEPENDS "${HFS_FIRMWARE_ELF}")
add_custom_command(TARGET hfs-driver POST_BUILD
  COMMAND "${CMAKE_OBJCOPY}" --add-section
    ".infiltrator_boot=${HFS_FIRMWARE_ELF}"
    --set-section-flags .infiltrator_boot=readonly
    "$<TARGET_FILE:hfs-driver>"
  VERBATIM)

if(BUILD_TESTING)
  add_executable(hfs-real-media-test tests/test_real_media.c)
  target_include_directories(hfs-real-media-test PRIVATE
    "${infiltratr_common_SOURCE_DIR}/include")
  target_compile_options(hfs-real-media-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_link_libraries(hfs-real-media-test PRIVATE
    hfs-driver InfiltratrCommon::Common)
  option(HFS_REAL_MEDIA_TESTS "Test third-party formatted HFS media" OFF)
  if(HFS_REAL_MEDIA_TESTS)
    find_program(HFS_FORMAT_TOOL hformat REQUIRED)
    find_program(HFS_COPY_TOOL hcopy REQUIRED)
    add_test(NAME hfs-common-v2-independent-real-media
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/real-hfs-v2.sh"
        "$<TARGET_FILE:hfs-real-media-test>")
  endif()

  add_executable(hfs-native-format-test tests/test_format.c)
  target_link_libraries(hfs-native-format-test PRIVATE InfiltratrCommon::Common ${CMAKE_DL_LIBS})
  target_compile_options(hfs-native-format-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  find_program(HFS_FORMAT_ORACLE NAMES hmount)
  if(HFS_FORMAT_ORACLE)
    add_test(NAME hfs-native-format COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/format_oracle.sh"
        "$<TARGET_FILE:hfs-native-format-test>" "$<TARGET_FILE:hfs-driver>" "${HFS_FORMAT_ORACLE}")
  else()
    add_test(NAME hfs-native-format COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/format_oracle.sh"
        "$<TARGET_FILE:hfs-native-format-test>" "$<TARGET_FILE:hfs-driver>")
  endif()
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
  # Compile the identical HFS directory parser used by both Common V2
  # profiles and refuse duplicate case-folded parent/name keys.
  add_executable(hfs-duplicate-catalogue-test
    tests/test_duplicate_catalogue.c)
  target_include_directories(hfs-duplicate-catalogue-test PRIVATE
    "${infiltratr_common_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/native")
  target_compile_options(hfs-duplicate-catalogue-test PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME hfs-v2-duplicate-catalogue-corruption
    COMMAND hfs-duplicate-catalogue-test)
endif()
install(TARGETS hfs-driver LIBRARY DESTINATION /System/Filesystems)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/HFS.pdf"
  DESTINATION /System/Resources/Documentation
  RENAME Filesystem-HFS.pdf)
