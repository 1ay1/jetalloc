// MUST NOT COMPILE: taking a lower rank (10) AFTER a higher rank (20) is a
// lock-order violation — the potential-ABBA-deadlock guarantee is a compile
// error, not a comment.
#include "jetalloc_sync.hpp"
void f() {
    // outer rank 20, inner rank 10 → inner must be strictly greater. static_assert fires.
    jet::assert_lock_order<20, 10>();
}
