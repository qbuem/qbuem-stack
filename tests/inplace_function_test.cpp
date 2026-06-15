// Unit tests for qbuem::inplace_function — the zero-allocation std::function
// alternative for hot-path callbacks.
#include <gtest/gtest.h>
#include <qbuem/buf/inplace_function.hpp>

#include <array>
#include <cstdint>

using qbuem::inplace_function;

TEST(InplaceFunction, CallsAndReturnsValue) {
    inplace_function<int(int, int)> add = [](int a, int b) { return a + b; };
    EXPECT_TRUE(static_cast<bool>(add));
    EXPECT_EQ(add(2, 3), 5);
    EXPECT_EQ(add(-4, 4), 0);
}

TEST(InplaceFunction, CapturesAndMutatesState) {
    int calls = 0;
    inplace_function<void()> tick = [&calls]() mutable { ++calls; };
    tick(); tick(); tick();
    EXPECT_EQ(calls, 3);
}

TEST(InplaceFunction, VoidSignatureWithArgs) {
    int last = -1;
    inplace_function<void(int)> sink = [&last](int v) { last = v; };
    sink(42);
    EXPECT_EQ(last, 42);
}

TEST(InplaceFunction, DefaultIsEmpty) {
    inplace_function<void()> f;
    EXPECT_FALSE(static_cast<bool>(f));
    f = nullptr;
    EXPECT_FALSE(static_cast<bool>(f));
}

TEST(InplaceFunction, MoveTransfersTarget) {
    inplace_function<int()> a = []() { return 7; };
    inplace_function<int()> b = std::move(a);
    EXPECT_FALSE(static_cast<bool>(a));   // moved-from is empty
    EXPECT_TRUE(static_cast<bool>(b));
    EXPECT_EQ(b(), 7);
}

TEST(InplaceFunction, MoveAssignReplacesAndDestroysOld) {
    // Track destruction of the captured object to prove no leak / correct dtor.
    struct Tracker {
        int* destroyed;
        Tracker(int* d) : destroyed(d) {}
        Tracker(Tracker&& o) noexcept : destroyed(o.destroyed) { o.destroyed = nullptr; }
        ~Tracker() { if (destroyed) ++*destroyed; }
        int operator()() const { return 1; }
    };
    int destroyed = 0;
    {
        inplace_function<int()> f = Tracker{&destroyed};
        inplace_function<int()> g = Tracker{&destroyed};
        f = std::move(g);   // old target of f destroyed here
        EXPECT_GE(destroyed, 1);
        EXPECT_EQ(f(), 1);
    }
    // both live targets destroyed by end of scope
    EXPECT_GE(destroyed, 2);
}

TEST(InplaceFunction, HoldsLargeCaptureWithoutHeap) {
    // A capture near the inline capacity must still fit (compile-time enforced).
    // 40 bytes of capture > std::function's SBO (~16B) — std::function would
    // heap-allocate here; inplace_function stores it inline.
    std::array<std::uint64_t, 4> payload{1, 2, 3, 4};  // 32 bytes
    std::uint64_t base = 100;
    inplace_function<std::uint64_t(), 64> f =
        [payload, base]() { return payload[0] + payload[3] + base; };
    EXPECT_EQ(f(), 1u + 4u + 100u);
}

TEST(InplaceFunction, SizeIsInlineBufferPlusVtable) {
    // No hidden heap pointer: the object IS the inline buffer + 3 fn pointers,
    // rounded up to the type's alignment. The default Align is
    // alignof(std::max_align_t), which is 16 on x86_64 (16-byte long double) but
    // 8 on some ABIs (e.g. Apple arm64); the 72-byte payload therefore pads to
    // 80 where alignment is 16. Compute the expected size from alignof(F) so the
    // "no hidden state" invariant holds portably instead of hardcoding 72.
    using F = inplace_function<void(), 48>;
    constexpr std::size_t payload  = 48u + 3u * sizeof(void*);
    constexpr std::size_t expected = (payload + alignof(F) - 1) / alignof(F) * alignof(F);
    EXPECT_EQ(sizeof(F), expected);
}
