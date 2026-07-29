// MUST NOT COMPILE: a lock guard is move-only. Copying it would mean two owners
// of one lock — you cannot alias lock ownership.
#include "jetalloc_sync.hpp"
void f() {
    jet::guarded<int> g{0};
    auto a = g.lock();
    auto b = a;            // error: guard copy ctor is deleted
    (void)b;
}
