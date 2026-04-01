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
# Since your buffer is header-only (template), INTERFACE is correct.
add_library(llb_core INTERFACE)

# This tells anyone linking to llb_core where to find the headers
target_include_directories(llb_core INTERFACE 
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# --- 3. Compile Options (Strict for Quants) ---
target_compile_options(llb_core INTERFACE
    $<$<CONFIG:Release>:-O3 ${ARCH_FLAGS} -ffast-math -flto>
    $<$<CONFIG:Debug>:-g -O0 -Wall -Wextra -Wpedantic -Wshadow>
)

# --- 4. Threading Support ---
# Crucial for SPSC: find and link the system threading library
find_package(Threads REQUIRED)

# --- 5. Executables ---
# We create the benchmark app and link it to our core library
add_executable(LowLatencyBuffer main.cpp src/slab_allocator.cpp)

# Linking llb_core gives main.cpp access to the include/ folder and the O3 flags
target_link_libraries(LowLatencyBuffer PRIVATE llb_core Threads::Threads)

# --- 6. Future-Proofing Subdirectories ---
# Uncomment these as you add files to those folders
# add_subdirectory(src)
# add_subdirectory(apps)
# add_subdirectory(tests)
# add_subdirectory(benchmarks)