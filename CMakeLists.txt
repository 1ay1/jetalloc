cmake_minimum_required(VERSION 3.16)

# NB: CMAKE_SYSTEM_* are only populated AFTER project(), so the ASM decision
# happens below, once the platform is known.
project(jetalloc LANGUAGES C CXX VERSION 0.1.0
        DESCRIPTION "A fast, modern, drop-in memory allocator")

# ── Portability: the ASM per-CPU (rseq) fast path is x86-64 + Linux only. ──
# Everywhere else jetalloc runs on its portable C11 core (per-thread heaps +
# slabs); the per-CPU cache compiles to no-op stubs. So only enable the ASM
# language where we actually have an .S file to assemble — otherwise a Windows
# / macOS / arm64 configure would fail just trying to find an assembler.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux"
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
    enable_language(ASM)
    set(JET_HAVE_RSEQ_ASM ON)
else()
    set(JET_HAVE_RSEQ_ASM OFF)
endif()

# Detect whether we're the top-level project or a nested add_subdirectory().
# When embedded (agentty pulls us in as a submodule), we build ONLY the static
# library — no tests, no benches, no LD_PRELOAD shared lib — so a consumer gets
# exactly one clean target: `jetalloc`.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.21)
    set(JET_IS_TOPLEVEL ${PROJECT_IS_TOP_LEVEL})
else()
    if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
        set(JET_IS_TOPLEVEL ON)
    else()
        set(JET_IS_TOPLEVEL OFF)
    endif()
endif()

option(JET_BUILD_TESTS   "Build jetalloc tests"           ${JET_IS_TOPLEVEL})
option(JET_BUILD_BENCH   "Build jetalloc benchmarks"      ${JET_IS_TOPLEVEL})
option(JET_BUILD_PRELOAD "Build the LD_PRELOAD shared lib" ${JET_IS_TOPLEVEL})

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(JET_IS_TOPLEVEL AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Aggressive but safe optimisation for the allocator hot paths. Every flag is
# feature-checked so unsupported compilers (MSVC, older GCC/Clang) just skip it
# rather than erroring — keeping the "compiles anywhere" promise.
include(CheckCCompilerFlag)
set(JET_CORE_FLAGS "")
if(NOT MSVC)
    foreach(_flag
            -fno-plt
            -fno-strict-aliasing
            # initial-exec TLS: the thread-heap pointer becomes a direct
            # %fs-relative load instead of a __tls_get_addr() CALL on every
            # malloc/free. Valid because our thread-locals are defined IN the
            # allocator image (linked statically or LD_PRELOAD'd), never
            # dlopen'd late. This is one of the single biggest perf wins.
            -ftls-model=initial-exec
            -fvisibility=hidden)
        string(MAKE_C_IDENTIFIER "JET_HAS${_flag}" _var)
        check_c_compiler_flag(${_flag} ${_var})
        if(${_var})
            list(APPEND JET_CORE_FLAGS ${_flag})
        endif()
    endforeach()
    list(APPEND JET_CORE_FLAGS -Wall -Wextra $<$<CONFIG:Release>:-O3>)
else()
    # MSVC's <stdatomic.h> (used across jet_central/jet_epoch/jet_percpu for
    # the lock-free counters) is gated behind an opt-in: without it the header
    # hard-errors with C1189 '"C atomic support is not enabled"'. Enable the
    # C11 atomics feature (VS 2022 17.5+) and pin C11 so the compiler actually
    # honours _Atomic / <stdatomic.h>. Applies to the C TUs only — the C++
    # override TU uses std::atomic and needs neither.
    list(APPEND JET_CORE_FLAGS
        $<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>
        $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()

# ── Core static library ──────────────────────────────────────────────────
set(JET_CORE_SOURCES
    src/jet_os.c
    src/jet_size.c
    src/jet_central.c
    src/jet_large.c
    src/jet_percpu.c
    src/jet_epoch.c
    src/jet_numa.c
    src/jet_core.c)
if(JET_HAVE_RSEQ_ASM)
    list(APPEND JET_CORE_SOURCES src/jet_rseq.S)
endif()

add_library(jetalloc STATIC ${JET_CORE_SOURCES} src/jet_override.cpp)
target_compile_options(jetalloc PRIVATE ${JET_CORE_FLAGS})
target_include_directories(jetalloc PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_include_directories(jetalloc PRIVATE src)
find_package(Threads REQUIRED)
target_link_libraries(jetalloc PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

# ── LD_PRELOAD shared library: core + FULL C malloc-family interposition. ──
if(JET_BUILD_PRELOAD AND NOT WIN32)
    add_library(jetalloc_preload SHARED
        ${JET_CORE_SOURCES} src/jet_override.c src/jet_override.cpp)
    target_compile_options(jetalloc_preload PRIVATE ${JET_CORE_FLAGS})
    target_include_directories(jetalloc_preload PRIVATE src)
    target_include_directories(jetalloc_preload PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
    target_link_libraries(jetalloc_preload PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
    set_target_properties(jetalloc_preload PROPERTIES OUTPUT_NAME jetalloc)
    # LTO so the interposed malloc/free/calloc collapse the whole fast path into
    # a single leaf function — no cross-TU PLT/GOT call to jet_malloc/jet_free/
    # jet_owns on every allocation.
    if(NOT MSVC)
        set_target_properties(jetalloc_preload PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()

# ── Tests ────────────────────────────────────────────────────────────────
if(JET_BUILD_TESTS)
    enable_testing()
    add_executable(jet_test test/jet_test.c)
    target_link_libraries(jet_test PRIVATE jetalloc)
    add_test(NAME jet_test COMMAND jet_test)

    add_executable(jet_test_cpp test/jet_test.cpp)
    target_link_libraries(jet_test_cpp PRIVATE jetalloc)
    # NB: the *test* TU is pinned to -O2. GCC 16.1.1 miscompiles the specific
    # vector<string> + unordered_map<int,string> + to_string mix in this test at
    # -O3 (reproduces with the SYSTEM allocator too — it is a compiler bug, not
    # a jetalloc bug). The allocator library itself is built at -O3.
    target_compile_options(jet_test_cpp PRIVATE $<$<CONFIG:Release>:-O2>)
    add_test(NAME jet_test_cpp COMMAND jet_test_cpp)

    # The modern C++23 header (jetalloc.hpp). Compiled at C++23 specifically —
    # the library default is C++17 and the header degrades to it, but the test
    # exercises the full concept-constrained surface, so pin the newer
    # standard here. Same -O2 pin as jet_test_cpp for the GCC 16 -O3 miscompile.
    add_executable(jet_test_hpp test/jet_test_hpp.cpp)
    target_link_libraries(jet_test_hpp PRIVATE jetalloc)
    target_compile_features(jet_test_hpp PRIVATE cxx_std_23)
    target_compile_options(jet_test_hpp PRIVATE $<$<CONFIG:Release>:-O2>)
    add_test(NAME jet_test_hpp COMMAND jet_test_hpp)

    add_executable(jet_test_mt test/jet_test_mt.c)
    target_link_libraries(jet_test_mt PRIVATE jetalloc Threads::Threads)
    add_test(NAME jet_test_mt COMMAND jet_test_mt)

    add_executable(jet_test_xthread test/jet_test_xthread.c)
    target_link_libraries(jet_test_xthread PRIVATE jetalloc Threads::Threads)
    add_test(NAME jet_test_xthread COMMAND jet_test_xthread)

    add_executable(jet_test_large_mt test/jet_test_large_mt.c)
    target_link_libraries(jet_test_large_mt PRIVATE jetalloc Threads::Threads)
    add_test(NAME jet_test_large_mt COMMAND jet_test_large_mt)
endif()

# ── Benchmark ────────────────────────────────────────────────────────────
if(JET_BUILD_BENCH)
    add_executable(jet_bench bench/jet_bench.c)
    target_link_libraries(jet_bench PRIVATE jetalloc Threads::Threads)

    # System-allocator baseline (same source, no jetalloc link).
    add_executable(jet_bench_system bench/jet_bench.c)
    target_compile_definitions(jet_bench_system PRIVATE JET_BENCH_SYSTEM=1)
    target_link_libraries(jet_bench_system PRIVATE Threads::Threads)
endif()

# ── Install ──────────────────────────────────────────────────────────────
if(JET_IS_TOPLEVEL)
    include(GNUInstallDirs)
    set(_jet_install_targets jetalloc)
    if(TARGET jetalloc_preload)
        list(APPEND _jet_install_targets jetalloc_preload)
    endif()
    install(TARGETS ${_jet_install_targets}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(FILES include/jetalloc.h include/jetalloc.hpp
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()
