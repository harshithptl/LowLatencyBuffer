cmake_minimum_required(VERSION 3.25)
project(LowLatencyBuffer 
        VERSION 0.1.0 
        DESCRIPTION "High-performance SPSC buffer for exchange infrastructure"
        LANGUAGES CXX)

# --- 1. Compiler Settings (C++26 Ready) ---
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# M4 Specific Performance Flags
if(APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
    set(ARCH_FLAGS "-mcpu=apple-m4")
else()
    set(ARCH_FLAGS "-march=native")
endif()

# --- 2. Define the Library Target ---
# We define our core logic as a library so it can be used by both apps and tests
add_library(llb_core INTERFACE)
target_include_directories(llb_core INTERFACE 
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# --- 3. Compile Options (Strict for Quants) ---
target_compile_options(llb_core INTERFACE
    $<$<CONFIG:Release>:-O3 ${ARCH_FLAGS} -ffast-math -flto>
    $<$<CONFIG:Debug>:-g -O0 -Wall -Wextra -Wpedantic -Wshadow>
)

# --- 4. Subdirectories ---
# We will add these as we build them
# add_subdirectory(src)
# add_subdirectory(apps)
# add_subdirectory(tests)
# add_subdirectory(benchmarks)
add_executable(LowLatencyBuffer main.cpp)