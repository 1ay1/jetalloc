// MUST NOT COMPILE: 48 is not a power of two → power_of_two<48> is ill-formed.
#include "jetalloc.hpp"
void f() {
    auto buf = jet::buffer::make(256, jet::power_of_two<48>{});   // error: static_assert
    (void)buf;
}
