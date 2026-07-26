cmake_minimum_required(VERSION 3.16)
project(jetalloc LANGUAGES C CXX ASM VERSION 0.1.0
        DESCRIPTION "A fast, modern, drop-in memory allocator")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Aggressive but safe optimisation for the allocator hot paths.
if(NOT MSVC)
    add_compile_options(
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:Release>:-fno-plt>
        -fno-strict-aliasing
        # initial-exec TLS: the thread-heap pointer becomes a direct %fs-relative
        # load instead of a __tls_get_addr() CALL on every malloc/free. Valid
        # because our thread-locals are defined IN the allocator image (whether
        # linked statically or LD_PRELOAD'd), never dlopen'd late.
        -ftls-model=initial-exec
        -fvisibility=hidden
        -Wall -Wextra)
endif()

# ── Core static + shared library ─────────────────────────────────────────
set(JET_CORE_SOURCES
    src/jet_os.c
    src/jet_size.c
    src/jet_central.c
    src/jet_large.c
    src/jet_percpu.c
    src/jet_epoch.c
    src/jet_numa.c
    src/jet_rseq.S
    src/jet_core.c)

add_library(jetalloc STATIC ${JET_CORE_SOURCES} src/jet_override.cpp)
target_include_directories(jetalloc PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_include_directories(jetalloc PRIVATE src)
find_package(Threads REQUIRED)
target_link_libraries(jetalloc PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

# Shared "preload" library: core + FULL C malloc-family interposition. This is
# the LD_PRELOAD target that replaces the system allocator process-wide.
add_library(jetalloc_preload SHARED
    ${JET_CORE_SOURCES} src/jet_override.c src/jet_override.cpp)
target_include_directories(jetalloc_preload PRIVATE src)
target_include_directories(jetalloc_preload PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(jetalloc_preload PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
set_target_properties(jetalloc_preload PROPERTIES OUTPUT_NAME jetalloc)

# ── Tests ────────────────────────────────────────────────────────────────
enable_testing()
add_executable(jet_test test/jet_test.c)
target_link_libraries(jet_test PRIVATE jetalloc)
add_test(NAME jet_test COMMAND jet_test)

add_executable(jet_test_cpp test/jet_test.cpp)
target_link_libraries(jet_test_cpp PRIVATE jetalloc)
# NB: the *test* TU is pinned to -O2. GCC 16.1.1 miscompiles the specific
# vector<string> + unordered_map<int,string> + to_string mix in this test at
# -O3 (reproduces with the SYSTEM allocator too — it is a compiler bug, not a
# jetalloc bug). The allocator library itself is built at -O3.
target_compile_options(jet_test_cpp PRIVATE $<$<CONFIG:Release>:-O2>)
add_test(NAME jet_test_cpp COMMAND jet_test_cpp)

add_executable(jet_test_mt test/jet_test_mt.c)
target_link_libraries(jet_test_mt PRIVATE jetalloc Threads::Threads)
add_test(NAME jet_test_mt COMMAND jet_test_mt)

add_executable(jet_test_xthread test/jet_test_xthread.c)
target_link_libraries(jet_test_xthread PRIVATE jetalloc Threads::Threads)
add_test(NAME jet_test_xthread COMMAND jet_test_xthread)

# ── Benchmark ────────────────────────────────────────────────────────────
add_executable(jet_bench bench/jet_bench.c)
target_link_libraries(jet_bench PRIVATE jetalloc Threads::Threads)

# System-allocator baseline (same source, no jetalloc link).
add_executable(jet_bench_system bench/jet_bench.c)
target_compile_definitions(jet_bench_system PRIVATE JET_BENCH_SYSTEM=1)
target_link_libraries(jet_bench_system PRIVATE Threads::Threads)

# ── Install ──────────────────────────────────────────────────────────────
include(GNUInstallDirs)
install(TARGETS jetalloc jetalloc_preload
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/jetalloc.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
