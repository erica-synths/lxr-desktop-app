# Copyright (C) 2026 Erica Synths
# SPDX-License-Identifier: GPL-3.0-only

# Host unit tests: firmware sources compiled with the computer's own C compiler
# and run on the computer. See README.md in this folder.
#
#   cmake --preset host-tests
#   cmake --build --preset host-tests
#   ctest --preset host-tests

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

set(LXR_SOURCE_DIR "${PROJECT_SOURCE_DIR}")

# AddressSanitizer and UndefinedBehaviorSanitizer turn out-of-bounds accesses
# and undefined behaviour in the code under test into test failures.
option(LXR_TEST_SANITIZERS "Build the host tests with ASan and UBSan" ON)

add_library(lxr_test_common INTERFACE)
target_include_directories(lxr_test_common INTERFACE
    # Must come before Core/Inc: replaces the ST HAL and CMSIS headers.
    ${CMAKE_CURRENT_SOURCE_DIR}/shim
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
    ${LXR_SOURCE_DIR}/Core/Inc
    ${LXR_SOURCE_DIR}/FATFS/App
    ${LXR_SOURCE_DIR}/FATFS/Target
    ${LXR_SOURCE_DIR}/Middlewares/Third_Party/FatFs/src
)
target_compile_definitions(lxr_test_common INTERFACE LXR_HOST_TEST)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(lxr_test_common INTERFACE -g -O1 -Wall)
    # The firmware uses GCC's "#pragma GCC optimize", which Clang doesn't know.
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        target_compile_options(lxr_test_common INTERFACE -Wno-unknown-pragmas)
    endif()
    if(LXR_TEST_SANITIZERS)
        set(sanitize -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
        target_compile_options(lxr_test_common INTERFACE ${sanitize})
        target_link_options(lxr_test_common INTERFACE ${sanitize})
    endif()
endif()

# lxr_add_host_test(<name> SOURCES <test and firmware sources...>)
#
# Builds one test program from its test file, the firmware files it tests
# (paths relative to the repository root) and any stubs, and registers it with
# CTest.
function(lxr_add_host_test name)
    cmake_parse_arguments(arg "" "" "SOURCES;FIRMWARE_SOURCES" ${ARGN})
    list(TRANSFORM arg_FIRMWARE_SOURCES PREPEND "${LXR_SOURCE_DIR}/")
    add_executable(${name} ${arg_SOURCES} ${arg_FIRMWARE_SOURCES})
    target_link_libraries(${name} PRIVATE lxr_test_common)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

lxr_add_host_test(test_fifo
    SOURCES test_fifo.c
    FIRMWARE_SOURCES Core/Src/Soundengine/FIFO.c
)

lxr_add_host_test(test_euklid
    SOURCES test_euklid.c
    FIRMWARE_SOURCES Core/Src/Soundengine/Sequencer/EuklidGenerator.c
)

lxr_add_host_test(test_frontpanel_cc
    SOURCES test_frontpanel_cc.c
    FIRMWARE_SOURCES Core/Src/frontPanelParser.c
)
