# expect_fail.cmake — compile SRC and assert it FAILS. Used for negative tests:
# proof that a memory-unsafe operation does not compile. Compiler-aware so it
# works with GCC/Clang (-std/-fsyntax-only) and MSVC (/std /Zs).
if(CXXID STREQUAL "MSVC")
    execute_process(
        COMMAND ${CXX} /std:c++latest /Zs /EHsc /I${JETINC} ${SRC}
        RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
else()
    execute_process(
        COMMAND ${CXX} -std=c++23 -pthread -fsyntax-only -I${JETINC} ${SRC}
        RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
endif()

if(rc EQUAL 0)
    message(FATAL_ERROR "NEGATIVE TEST FAILED: ${SRC} compiled but MUST NOT")
endif()
message(STATUS "ok: ${SRC} correctly rejected by the compiler")
