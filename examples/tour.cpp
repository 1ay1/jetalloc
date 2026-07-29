// ╔══════════════════════════════════════════════════════════════════════════╗
// ║  jetalloc — a guided tour you can run.                                     ║
// ║      cmake --build build --target jet_tour && ./build/jet_tour            ║
// ║  Every line below is a guarantee you'd normally need Rust for. The three  ║
// ║  commented-out lines at the bottom are the whole point: they DON'T        ║
// ║  compile. Uncomment one and watch the type system reject the bug.         ║
// ╚══════════════════════════════════════════════════════════════════════════╝
#include <jetalloc.hpp>
#include <cstdio>
#include <string>
#include <vector>

using namespace jet;

namespace {
int step = 0;
void say(const char* what) { std::printf("  %2d ▸ %s\n", ++step, what); }
}

struct Widget {
    std::string name;
    int hp;
    Widget(std::string n, int h) : name(std::move(n)), hp(h) {}
    void hit(int d) { hp -= d; }
};

int main() {
    std::printf("\n  jetalloc v%s — guided tour\n", version());
    std::printf("  ─────────────────────────────────────────────\n");

    // ── Ownership: an affine, RAII handle. No new, no delete. ────────────────
    owned<Widget> w = make<Widget>("goblin", 30);
    say("made an owned<Widget> — no `new`, and its destructor will free it");

    // ── Scoped borrows: &mut and & with Rust's exact aliasing rule. ──────────
    w.with_mut([](Widget& g) { g.hit(12); });
    int hp = w.with([](const Widget& g) { return g.hp; });
    say(("scoped &mut then & — goblin hp is now " + std::to_string(hp)).c_str());

    // ── Move is a transfer of ownership; the source becomes statically dead. ─
    owned<Widget> stolen = std::move(w);
    say("moved ownership — the old handle is null, using it would be a no-op");

    // ── OOM is a value (std::expected), not a crash or a null you forget. ────
    result<owned<int>> maybe = try_make<int>(7);
    if (maybe) say("try_make<int> returned a value; OOM would've been Err, not UB");

    // ── Bounded arrays: elements only via std::span or checked .at(). ────────
    auto squares = make_array_with<int>(6, [](std::size_t i) { return int(i * i); });
    std::string row;
    for (int n : squares.view()) row += std::to_string(n) + " ";
    say(("bounded array via std::span → " + row + "(no raw pointer escaped)").c_str());

    // ── Raw, over-aligned bytes — alignment PROVEN at compile time. ──────────
    auto page = buffer::make(256, power_of_two<64>{});
    bool aligned = (reinterpret_cast<std::uintptr_t>(page.bytes().data()) % 64) == 0;
    say(aligned ? "buffer::make(256, power_of_two<64>) — 64-byte aligned, proven"
                : "alignment check failed (should be impossible)");

    // ── Drop-in std::allocator: containers get jetalloc for free. ────────────
    std::vector<int, jet::allocator<int>> v;
    for (int i = 0; i < 1000; ++i) v.push_back(i);
    say("std::vector<int, jet::allocator<int>> filled 1000 elems on jetalloc");

    std::printf("  ─────────────────────────────────────────────\n");
    std::printf("  ✓ every handle above frees itself. No leaks. No delete.\n\n");

    // ── The bugs that DON'T COMPILE. Uncomment any one to see for yourself. ──
    //   owned<Widget> copy = stolen;          // ✗ deleted copy ctor (double free)
    //   int x = *stolen;                       // ✗ owned<T> has no operator*
    //   auto bad = buffer::make(16, power_of_two<48>{}); // ✗ 48 isn't a power of two
    return 0;
}
