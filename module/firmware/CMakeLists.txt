# SPDX-License-Identifier: GPL-3.0-or-later
# Identical HFS format-owned parser and V2 module for UEFI and hosted profiles.
cmake_minimum_required(VERSION 3.20)
project(ClassicHfsFirmware LANGUAGES C)
set(COMMON_INCLUDE_DIR "" CACHE PATH "Pinned Common V2 include directory")
if(NOT EXISTS "${COMMON_INCLUDE_DIR}/infiltratr/filesystem_module_v2.h")
  message(FATAL_ERROR "Common V2 header required")
endif()
add_library(hfs-firmware MODULE
  ../../native/hfs_catalog_v2.c
  ../../module/hfs_module.c
  runtime.c)
set_target_properties(hfs-firmware PROPERTIES
  PREFIX "" OUTPUT_NAME "HFS" SUFFIX ".so" C_VISIBILITY_PRESET hidden)
target_include_directories(hfs-firmware PRIVATE
  "${COMMON_INCLUDE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../native")
target_compile_features(hfs-firmware PRIVATE c_std_11)
target_compile_definitions(hfs-firmware PRIVATE IFS_HFS_FIRMWARE_BUILD=1)
# Firmware uses AMD64 SysV without CET, SIMD state assumptions,
# packed relative relocations or non-4-KiB load alignment. Hosted builds
# retain their separate native optimization profile.
target_compile_options(hfs-firmware PRIVATE
  -O2 -Wall -Wextra -Wpedantic -Werror
  -ffreestanding -fno-builtin -fno-stack-protector
  -ffunction-sections -fdata-sections -mno-red-zone -fcf-protection=none -mgeneral-regs-only
  -fno-tree-loop-distribute-patterns -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
target_link_options(hfs-firmware PRIVATE
  -nostdlib -Wl,-z,nopack-relative-relocs,-z,max-page-size=4096,-z,defs,-z,noexecstack,-z,relro,-z,now,--gc-sections,-Bsymbolic)
