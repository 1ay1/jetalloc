// MUST NOT COMPILE: a unique borrow (mut / &mut) is move-only, never copyable.
#include "jetalloc.hpp"
void f() {
    auto o = jet::make<int>(1);
    auto m = o.borrow_mut();
    auto m2 = m;             // error: mut copy ctor is deleted
    (void)m2;
}
