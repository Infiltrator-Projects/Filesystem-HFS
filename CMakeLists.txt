cmake_minimum_required(VERSION 3.20)
project(FilesystemHFS VERSION 1.0.0 LANGUAGES C)
include(CTest)
include(FetchContent)
find_package(OpenSSL REQUIRED)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(INFILTRATR_COMMON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(INFILTRATR_COMMON_BUILD_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_Declare(infiltratr_common GIT_REPOSITORY https://github.com/Infiltrator-Projects/Libraries.git
 GIT_TAG c76ec95ef66fc728df6a906ad75a6a4dd63b419b)
FetchContent_MakeAvailable(infiltratr_common)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/version.h" "#define LD_VERSION \"1.0.0\"\n")
add_library(hfs-driver SHARED native/writer.c
 compat/legacy_services.c module/hfs_module.c
 fixtures/hfs_codec.c module/hfs_fixture_bridge.c)
set_source_files_properties(fixtures/hfs_codec.c PROPERTIES
 COMPILE_DEFINITIONS "ldtm_raw_mac=ifs_hfs_raw_fixture")
set_target_properties(hfs-driver PROPERTIES PREFIX "" OUTPUT_NAME "HFS" SUFFIX ".so")
target_compile_definitions(hfs-driver PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
target_compile_options(hfs-driver PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_options(hfs-driver PRIVATE -Wl,-z,defs)
target_include_directories(hfs-driver PRIVATE
 "${CMAKE_CURRENT_SOURCE_DIR}/native" "${CMAKE_CURRENT_SOURCE_DIR}/compat"
 "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/include"
 "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(hfs-driver PRIVATE InfiltratrCommon::Common OpenSSL::Crypto)
if(BUILD_TESTING)
 add_executable(hfs-module-test tests/test_module.c)
 target_include_directories(hfs-module-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
 target_link_libraries(hfs-module-test PRIVATE hfs-driver)
 add_test(NAME hfs-module-interface COMMAND hfs-module-test)
endif()
install(TARGETS hfs-driver LIBRARY DESTINATION /System/Filesystems)
