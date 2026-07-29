cmake_minimum_required(VERSION 3.16)

project(jetalloc LANGUAGES CXX VERSION 0.3.0
        DESCRIPTION "A header-only, type-theoretic, memory-safe allocator")

# ── Header-only INTERFACE library ────────────────────────────────────────────
# There is nothing to compile: jetalloc is one header. Consumers just:
#     target_link_libraries(app PRIVATE jetalloc)
# and get the include path + a C++23 requirement, no object files, no linking.
add_library(jetalloc INTERFACE)
add_library(jetalloc::jetalloc ALIAS jetalloc)

target_include_directories(jetalloc INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(jetalloc INTERFACE cxx_std_23)

# Detect top-level vs. embedded (agentty pulls us in as a submodule). When
# embedded we expose ONLY the interface target — no tests.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.21)
    set(JET_IS_TOPLEVEL ${PROJECT_IS_TOP_LEVEL})
else()
    if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
        set(JET_IS_TOPLEVEL ON)
    else()
        set(JET_IS_TOPLEVEL OFF)
    endif()
endif()

# ── Tests (top-level only) ───────────────────────────────────────────────────
if(JET_IS_TOPLEVEL)
    enable_testing()

    # Opt-in sanitizers for the test binaries (ASan+UBSan, or TSan for the MT
    # test). -DJET_SANITIZE=address|thread|undefined  (comma-joined ok).
    set(JET_SANITIZE "" CACHE STRING "Sanitizers for jetalloc tests (e.g. address,undefined or thread)")
    set(_jet_san_flags "")
    if(JET_SANITIZE)
        set(_jet_san_flags -fsanitize=${JET_SANITIZE} -fno-omit-frame-pointer -g)
    endif()

    add_executable(jet_test test/jet_test.cpp)
    target_link_libraries(jet_test PRIVATE jetalloc)
    target_compile_options(jet_test PRIVATE $<$<CONFIG:Release>:-O2> ${_jet_san_flags})
    target_link_options(jet_test PRIVATE ${_jet_san_flags})
    add_test(NAME jet_test COMMAND jet_test)

    # Multithreaded cross-thread-free stress test (needs a thread library).
    find_package(Threads REQUIRED)
    add_executable(jet_test_mt test/jet_test_mt.cpp)
    target_link_libraries(jet_test_mt PRIVATE jetalloc Threads::Threads)
    target_compile_options(jet_test_mt PRIVATE $<$<CONFIG:Release>:-O2> ${_jet_san_flags})
    target_link_options(jet_test_mt PRIVATE ${_jet_san_flags})
    add_test(NAME jet_test_mt COMMAND jet_test_mt)

    # Compile-time-safe concurrency layer (jetalloc_sync.hpp): guarded<>,
    # scoped_thread, channel<>, ranked locks. Needs a thread library.
    add_executable(jet_test_sync test/jet_test_sync.cpp)
    target_link_libraries(jet_test_sync PRIVATE jetalloc Threads::Threads)
    target_compile_options(jet_test_sync PRIVATE $<$<CONFIG:Release>:-O2> ${_jet_san_flags})
    target_link_options(jet_test_sync PRIVATE ${_jet_san_flags})
    add_test(NAME jet_test_sync COMMAND jet_test_sync)

    # Negative compile tests: each of these MUST fail to compile. We invoke the
    # compiler at build time and assert failure — proof that misuse is rejected.
    # Memory-safety negatives + concurrency-safety negatives.
    foreach(neg double_free wrong_align copy_owned
                non_send_thread copy_guard bad_lock_order)
        add_test(NAME neg_${neg}
                 COMMAND ${CMAKE_COMMAND}
                         -DJETINC=${CMAKE_CURRENT_SOURCE_DIR}/include
                         -DCXX=${CMAKE_CXX_COMPILER}
                         -DCXXID=${CMAKE_CXX_COMPILER_ID}
                         -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/test/neg/${neg}.cpp
                         -P ${CMAKE_CURRENT_SOURCE_DIR}/test/expect_fail.cmake)
    endforeach()

    # Throughput benchmark (jetalloc vs the system allocator). Not a CTest test
    # (timing is machine-dependent); build + run it explicitly.
    add_executable(jet_bench bench/jet_bench.cpp)
    target_link_libraries(jet_bench PRIVATE jetalloc)
    target_compile_options(jet_bench PRIVATE
        $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:-O2>)
endif()

# ── Install (top-level only) ─────────────────────────────────────────────────
if(JET_IS_TOPLEVEL)
    include(GNUInstallDirs)
    install(TARGETS jetalloc EXPORT jetallocTargets)
    install(FILES include/jetalloc.hpp include/jetalloc_sync.hpp
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(EXPORT jetallocTargets
            NAMESPACE jetalloc::
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/jetalloc)
endif()
