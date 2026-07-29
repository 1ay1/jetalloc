// jet_test_hardened.cpp — proves the JET_HARDENED tier turns heap corruption
// into a defined, immediate abort instead of silent UB.
//
// Structure: the corruption cases abort() the process. Rather than let CTest
// interpret a crash exit code (which is platform-specific and CTest flags as an
// "exception"), the DRIVER (no args) re-spawns THIS binary once per case and
// asserts each child terminated abnormally *and* printed the guard diagnostic.
// The driver itself exits 0 cleanly, so CTest sees an ordinary pass.
// SPDX-License-Identifier: MIT
#ifndef JET_HARDENED
#define JET_HARDENED
#endif
#include "jetalloc.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static void* small() { return jet::detail::raw_allocate(32, alignof(std::max_align_t)); }
static void  freep(void* p) { jet::detail::raw_deallocate(p, 32, alignof(std::max_align_t)); }

// ── the corruption cases (each must abort under JET_HARDENED) ────────────────
static void case_double_free() {
    void* p = small();
    freep(p);
    freep(p);            // ✗ double free — must abort
}
static void case_interior_free() {
    char* p = static_cast<char*>(small());
    freep(p + 8);        // ✗ interior pointer, not a block start — must abort
}

// ── the good path: hardened build must still work perfectly ──────────────────
static int case_ok() {
    std::vector<std::string> v;
    for (int i = 0; i < 5000; ++i) v.emplace_back(static_cast<std::size_t>(i % 40), 'x');
    void* a = small(); void* b = small();
    freep(a); freep(b); freep(small());
    assert(v.size() == 5000);
    std::puts("jet_test_hardened: OK path passed (allocator works, hardened)");
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 2) {
        std::string m = argv[1];
#if defined(_WIN32)
        // Keep abort() non-interactive (no Windows error dialog) so the child
        // dies cleanly for the driver to observe.
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
        if (m == "double")   case_double_free();
        if (m == "interior") case_interior_free();
        // Reached only if the guard did NOT fire → signal failure to the driver.
        std::fprintf(stderr, "corruption '%s' was NOT detected\n", m.c_str());
        return 0;   // clean exit == guard failed
    }

    // Driver: prove the good path, then that each corruption case aborts.
    if (case_ok() != 0) return 1;

    const char* self = argv[0];
    const char* cases[] = {"double", "interior"};
    for (const char* c : cases) {
        std::string cmd = std::string("\"") + self + "\" " + c + " 2>&1";
        std::FILE* pipe =
#if defined(_WIN32)
            _popen(cmd.c_str(), "r");
#else
            popen(cmd.c_str(), "r");
#endif
        assert(pipe && "failed to spawn self");
        std::string out; char buf[256];
        while (std::fgets(buf, sizeof buf, pipe)) out += buf;
        int rc =
#if defined(_WIN32)
            _pclose(pipe);
#else
            pclose(pipe);
#endif
        // The child must have aborted (non-zero termination) AND printed the
        // guard message — and must NOT have printed the "not detected" line.
        bool caught = out.find("HEAP CORRUPTION detected") != std::string::npos;
        bool leaked = out.find("was NOT detected") != std::string::npos;
        std::printf("  case '%s': rc=%d caught=%d\n", c, rc, caught ? 1 : 0);
        if (!caught || leaked || rc == 0) {
            std::fprintf(stderr, "jet_test_hardened: case '%s' NOT guarded!\n", c);
            return 1;
        }
    }
    std::puts("jet_test_hardened: all corruption cases aborted as required");
    return 0;
}
