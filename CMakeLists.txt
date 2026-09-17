# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2021-2025. All rights reserved.

cmake_minimum_required(VERSION 3.10)
project(ub_bench C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_SKIP_RPATH TRUE)

if(CROSS_COMPILE)
    set(CMAKE_C_COMPILER "${CROSS_COMPILE}")
    message(STATUS "CMAKE_C_COMPILER: ${CROSS_COMPILE}")
endif()

set(UB_BENCH_C_FLAGS " -Wall -Werror -Wformat -Wfloat-equal -Wtrampolines -g -O2 \
-fno-strict-aliasing -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE -fPIC")

set(UB_BENCH_FLAGS_ARM64 " -march=armv8-a+crc -DUB_ARCH_ARM64")
set(UB_BENCH_FLAGS_x86_64 " -msse4.2 -DUB_ARCH_X86_64")
if("${X86_CROSS_COMPILATION}" STREQUAL "enable")
    set(UB_BENCH_FLAGS_x86_64 " -DUB_ARCH_X86_64")
    message(STATUS "x86 cross compilation, disabling msse4.2!")
endif()

if("${CMAKE_HOST_SYSTEM_PROCESSOR}" STREQUAL "aarch64")
    set(CMAKE_C_FLAGS "${UB_BENCH_C_FLAGS} ${UB_BENCH_FLAGS_ARM64}")
else()
    set(CMAKE_C_FLAGS "${UB_BENCH_C_FLAGS} ${UB_BENCH_FLAGS_x86_64}")
endif()

set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -rdynamic -pie \
-Wl,-z,noexecstack,-z,relro,-z,now")

if("${ASAN}" STREQUAL "enable")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address -fsanitize-recover=address \
-fsanitize=leak -fsanitize=undefined -fno-omit-frame-pointer")
    message(STATUS "ASAN enabled")
endif()

if("${PERF_CYCLE}" STREQUAL "enable")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DPERF_CYCLE_FLAG")
    message(STATUS "PERF_CYCLE enabled")
endif()

message(STATUS "CMAKE_C_FLAGS = ${CMAKE_C_FLAGS}")

find_library(URMA_LIB NAMES urma)
if(NOT URMA_LIB)
    message(FATAL_ERROR
        "liburma not found. Please install UMDK first.")
endif()
message(STATUS "Found urma: ${URMA_LIB}")

find_library(URMA_COMMON_LIB NAMES urma_common)
if(NOT URMA_COMMON_LIB)
    message(FATAL_ERROR
        "liburma_common not found. Please install UMDK first.")
endif()
message(STATUS "Found urma_common: ${URMA_COMMON_LIB}")

find_path(URMA_INCLUDE_DIR urma_api.h
    PATHS /usr/include/ub/umdk/urma
    NO_DEFAULT_PATH
)
if(NOT URMA_INCLUDE_DIR)
    message(FATAL_ERROR
        "urma headers not found. Please install UMDK first.")
endif()
message(STATUS "Found urma headers: ${URMA_INCLUDE_DIR}")

add_executable(ub_bench
    src/ub_bench.c
    src/ub_bench_hist.c
    src/ub_bench_run.c
    src/ub_bench_log.c
    src/ub_bench_mgmt.c
    src/ub_bench_mgmt_tcp.c
    src/ub_bench_mgmt_ub.c
    src/ub_bench_parameters.c
    src/ub_bench_resources.c
    src/ub_bench_run_test.c
)

target_include_directories(ub_bench
    PRIVATE
        ${URMA_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/include/ub
        ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(ub_bench
    PRIVATE
        ${URMA_LIB}
        ${URMA_COMMON_LIB}
        pthread
        m
)

install(TARGETS ub_bench
    DESTINATION /usr/bin
)
