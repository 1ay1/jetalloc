// MUST NOT COMPILE: passing a thread-unsafe (no_send) value across a thread
// boundary is rejected at the spawn site — a data race can never begin.
#include "jetalloc_sync.hpp"
struct ThreadBound : jet::no_send {};
void f() {
    ThreadBound tb;
    jet::scoped_thread t([](ThreadBound){}, tb);   // error: constraint not satisfied
}
