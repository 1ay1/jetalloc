// MUST NOT COMPILE: copying an owned<T> would duplicate ownership → double free.
#include "jetalloc.hpp"
void f() {
    auto a = jet::make<int>(1);
    auto b = a;              // error: owned copy ctor is deleted
    (void)b;
}
