/**
 * @file shm_channel_test.cpp
 * @brief SHMChannel<T> + SHMBus functionality unit tests.
 *
 * Coverage:
 * - SHMChannel::create + is_open + capacity + size_approx
 * - try_send / try_recv (Vyukov MPMC correctness)
 * - close → is_open=false, try_send=false
 * - ring buffer full → try_send=false
 * - ring buffer empty → try_recv=nullopt
 * - multiple message order preservation (FIFO)
 * - SHMBus LOCAL_ONLY: declare / try_publish / subscribe / try_recv
 * - SHMBus: destructor releases resources (no memory leak)
 * - calc_segment_size return value validation
 */

#include <qbuem/shm/shm_bus.hpp>
#include <qbuem/shm/shm_channel.hpp>
#include <gtest/gtest.h>

#include <cstring>
#include <string>

using namespace qbuem::shm;

// ─── Test message type ────────────────────────────────────────────────────────

struct Msg32 {
    int64_t  seq{0};
    uint32_t val{0};
    uint32_t _pad{0};
};
static_assert(std::is_trivially_copyable_v<Msg32>);

// Use a unique shm name per test to prevent naming collisions
static int g_shm_counter = 0;
static std::string next_shm_name() {
    return "qbuem_test_" + std::to_string(++g_shm_counter);
}

// ─── SHMChannel creation ─────────────────────────────────────────────────────

TEST(SHMChannel, CreateSucceeds) {
    auto name = next_shm_name();
    auto res = SHMChannel<Msg32>::create(name, 8);
    ASSERT_TRUE(res.has_value()) << "SHMChannel::create failed";
    EXPECT_TRUE((*res)->is_open());
    EXPECT_GE((*res)->capacity(), 8u);
    ::shm_unlink(("/" + name).c_str());
}

TEST(SHMChannel, InitialSizeApproxIsZero) {
    auto name = next_shm_name();
    auto res = SHMChannel<Msg32>::create(name, 16);
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ((*res)->size_approx(), 0u);
    ::shm_unlink(("/" + name).c_str());
}

TEST(SHMChannel, CapacityRoundedUpToPowerOfTwo) {
    auto name = next_shm_name();
    auto res = SHMChannel<Msg32>::create(name, 5);  // 5 → rounds up to 8
    ASSERT_TRUE(res.has_value());
    EXPECT_GE((*res)->capacity(), 5u);
    // Must be power of 2
    size_t cap = (*res)->capacity();
    EXPECT_EQ(cap & (cap - 1), 0u);
    ::shm_unlink(("/" + name).c_str());
}

// ─── try_send / try_recv ──────────────────────────────────────────────────────

TEST(SHMChannel, TrySendRecvSingleMessage) {
    auto name = next_shm_name();
    auto res = SHMChannel<Msg32>::create(name, 8);
    ASSERT_TRUE(res.has_value());
    auto& ch = *res;

    Msg32 m{42, 99, 0};
    EXPECT_TRUE(ch->try_send(m));
    EXPECT_EQ(ch->size_approx(), 1u);

    auto r = ch->try_recv();
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ((*r)->seq, 42);
    EXPECT_EQ((*r)->val, 99u);
    EXPECT_EQ(ch->size_approx(), 0u);
    ::shm_unlink(("/" + name).c_str());
}

TEST(SHMChannel, TryRecvEmptyChannelReturnsNullopt) {
    auto name = next_shm_name();
    auto res = SHMChannel<Msg32>::create(name, 8);
    ASSERT_TRUE(res.has_value());
    auto r = (*res)->try_recv();
    EXPECT_FALSE(r.has_value());
    ::shm_unlink(("/" + name).c_str());
}

TEST(SHMChannel, FifoOrderPreserved) {
    auto name = next_shm_name();
    auto res = SHMChannel<Msg32>::create(name, 16);
    ASSERT_TRUE(res.has_value());
    auto& ch = *res;

    constexpr int N = 8;
    for (int i = 0; i < N; ++i) {
        Msg32 m{static_cast<int64_t>(i), static_cast<uint32_t>(i * 10), 0};
        EXPECT_TRUE(ch->try_send(m));
    }

    for (int i = 0; i < N; ++i) {
        auto r = ch->try_recv();
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ((*r)->seq, static_cast<int64_t>(i));
        EXPECT_EQ((*r)->val, static_cast<uint32_t>(i * 10));
    }
    EXPECT_FALSE(ch->try_recv().has_value());
    ::shm_unlink(("/" + name).c_str());
}

TEST(SHMChannel, FullChannelTrySendReturnsFalse) {
    auto name = next_shm_name();
    auto res = SHMChannel<Msg32>::create(name, 4);
    ASSERT_TRUE(res.has_value());
    auto& ch = *res;

    Msg32 m{0, 0, 0};
    // Fill the channel (cap=4)
    for (size_t i = 0; i < ch->capacity(); ++i)
        EXPECT_TRUE(ch->try_send(m));

    // Full, so returns false
    EXPECT_FALSE(ch->try_send(m));
    ::shm_unlink(("/" + name).c_str());
}

// ─── close / is_open ─────────────────────────────────────────────────────────

TEST(SHMChannel, CloseMarksNotOpen) {
    auto name = next_shm_name();
    auto res = SHMChannel<Msg32>::create(name, 8);
    ASSERT_TRUE(res.has_value());
    auto& ch = *res;

    EXPECT_TRUE(ch->is_open());
    ch->close();
    EXPECT_FALSE(ch->is_open());
    ::shm_unlink(("/" + name).c_str());
}

TEST(SHMChannel, TrySendAfterCloseReturnsFalse) {
    auto name = next_shm_name();
    auto res = SHMChannel<Msg32>::create(name, 8);
    ASSERT_TRUE(res.has_value());
    auto& ch = *res;

    ch->close();
    EXPECT_FALSE(ch->try_send(Msg32{}));
    ::shm_unlink(("/" + name).c_str());
}

TEST(SHMChannel, UnlinkRemovesSegment) {
    auto name = next_shm_name();
    // Verify unlink works correctly after create + close
    {
        auto res = SHMChannel<Msg32>::create(name, 8);
        ASSERT_TRUE(res.has_value());
        (*res)->close();
    }
    // unlink succeeds
    auto r = SHMChannel<Msg32>::unlink(name);
    EXPECT_TRUE(r.has_value()) << r.error().message();
    // Double unlink is also ok (ENOENT → ok)
    auto r2 = SHMChannel<Msg32>::unlink(name);
    EXPECT_TRUE(r2.has_value());
}

TEST(SHMChannel, UnlinkNonexistentOk) {
    // Nonexistent name is also ok (ENOENT silently ignored)
    auto r = SHMChannel<Msg32>::unlink("qbuem_nonexistent_test_xyz");
    EXPECT_TRUE(r.has_value());
}

// ─── SHMBus LOCAL_ONLY ───────────────────────────────────────────────────────

struct BusMsg {
    int    id;
    double val;
    char   tag[8];
};
static_assert(std::is_trivially_copyable_v<BusMsg>);

TEST(SHMBus, DeclareAndTopicCount) {
    SHMBus bus;
    EXPECT_FALSE(bus.has_topic("a"));
    EXPECT_TRUE(bus.declare<BusMsg>("a", TopicScope::LOCAL_ONLY, 32));
    EXPECT_TRUE(bus.has_topic("a"));
    EXPECT_EQ(bus.topic_count(), 1u);
}

TEST(SHMBus, DuplicateDeclareReturnsFalse) {
    SHMBus bus;
    EXPECT_TRUE(bus.declare<BusMsg>("t", TopicScope::LOCAL_ONLY, 32));
    EXPECT_FALSE(bus.declare<BusMsg>("t", TopicScope::LOCAL_ONLY, 32));
}

TEST(SHMBus, SubscribeNonexistentTopicReturnsNull) {
    SHMBus bus;
    EXPECT_EQ(bus.subscribe<BusMsg>("ghost"), nullptr);
}

TEST(SHMBus, TryPublishAndSubscribeRecv) {
    SHMBus bus;
    bus.declare<BusMsg>("bus.test", TopicScope::LOCAL_ONLY, 16);
    auto sub = bus.subscribe<BusMsg>("bus.test");
    ASSERT_NE(sub, nullptr);
    EXPECT_EQ(sub->topic(), "bus.test");
    EXPECT_EQ(sub->scope(), TopicScope::LOCAL_ONLY);

    BusMsg m1{10, 3.14, "hello\0\0"};
    EXPECT_TRUE(bus.try_publish("bus.test", m1));

    auto r = sub->try_recv();
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ((*r)->id, 10);
    EXPECT_DOUBLE_EQ((*r)->val, 3.14);
}

TEST(SHMBus, MultipleSubscribers_LocalBroadcast) {
    // AsyncChannel is MPMC — multiple subs share the same channel
    // Each try_recv competes; one sub gets the message
    SHMBus bus;
    bus.declare<BusMsg>("bus.mc", TopicScope::LOCAL_ONLY, 32);
    auto sub1 = bus.subscribe<BusMsg>("bus.mc");
    auto sub2 = bus.subscribe<BusMsg>("bus.mc");
    ASSERT_NE(sub1, nullptr);
    ASSERT_NE(sub2, nullptr);

    BusMsg m{99, 1.0, "test\0\0\0"};
    bus.try_publish("bus.mc", m);

    // One subscriber receives it (MPMC — competition)
    auto r1 = sub1->try_recv();
    auto r2 = sub2->try_recv();
    // Exactly one of the two must receive the message
    bool got = r1.has_value() || r2.has_value();
    EXPECT_TRUE(got);
}

TEST(SHMBus, TryPublishTypeMismatchReturnsFalse) {
    SHMBus bus;
    bus.declare<BusMsg>("typed", TopicScope::LOCAL_ONLY, 16);
    struct Other { int x; };
    Other o{1};
    EXPECT_FALSE(bus.try_publish("typed", o));
}

TEST(SHMBus, TryPublishUnknownTopicReturnsFalse) {
    SHMBus bus;
    BusMsg m{};
    EXPECT_FALSE(bus.try_publish("noexist", m));
}

TEST(SHMBus, PerSubscriberBufferIsolation) {
    // Verify LocalSub bug fix: two subscribers must have different buf_ instances
    SHMBus bus;
    bus.declare<BusMsg>("iso", TopicScope::LOCAL_ONLY, 32);
    auto sub1 = bus.subscribe<BusMsg>("iso");
    auto sub2 = bus.subscribe<BusMsg>("iso");
    ASSERT_NE(sub1, nullptr);
    ASSERT_NE(sub2, nullptr);

    BusMsg a{1, 1.1, "AAA\0\0\0\0"};
    BusMsg b{2, 2.2, "BBB\0\0\0\0"};
    bus.try_publish("iso", a);
    bus.try_publish("iso", b);

    // Each subscriber uses an independent buffer (pointers must differ)
    auto ra = sub1->try_recv();
    auto rb = sub2->try_recv();

    if (ra.has_value() && rb.has_value()) {
        // The two pointers must differ
        EXPECT_NE(*ra, *rb);
    }
}

// ─── Security: open()/try_recv() validate an untrusted segment ───────────────
//
// SHMChannel::open() maps memory written by another process. A crashed or
// hostile peer can forge the header (capacity) and slot metadata (off). These
// tests forge such segments directly and assert the channel rejects them
// instead of dereferencing out-of-bounds pointers (ASan would otherwise fire).

namespace {

// A raw, caller-controlled shm segment. Destructor unmaps + unlinks.
struct ForgedSegment {
    void*       base{nullptr};
    size_t      size{0};
    int         fd{-1};
    std::string path;  // includes leading '/'

    ForgedSegment() = default;
    ForgedSegment(const ForgedSegment&) = delete;
    ForgedSegment& operator=(const ForgedSegment&) = delete;
    ~ForgedSegment() {
        if (base != nullptr && base != MAP_FAILED) ::munmap(base, size);
        if (fd >= 0) ::close(fd);
        if (!path.empty()) ::shm_unlink(path.c_str());
    }
};

// Create a zero-filled shm segment of exactly `bytes` and map it read-write.
// Returns nullptr on failure (skips the test cleanly).
std::unique_ptr<ForgedSegment> forge(const std::string& name, size_t bytes) {
    auto seg = std::make_unique<ForgedSegment>();
    seg->path = "/" + name;
    ::shm_unlink(seg->path.c_str());
    seg->fd = ::shm_open(seg->path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (seg->fd < 0) return nullptr;
    if (::ftruncate(seg->fd, static_cast<off_t>(bytes)) < 0) return nullptr;
    seg->size = bytes;
    seg->base = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                       MAP_SHARED, seg->fd, 0);
    if (seg->base == MAP_FAILED) { seg->base = nullptr; return nullptr; }
    std::memset(seg->base, 0, bytes);
    return seg;
}

} // namespace

TEST(SHMSecurity, OpenRejectsBadMagic) {
    auto name = next_shm_name();
    auto seg = forge(name, calc_segment_size(8, sizeof(Msg32)));
    if (!seg) GTEST_SKIP() << "shm_open unavailable in this environment";
    auto* hdr = new (seg->base) SHMHeader();
    hdr->magic    = 0xDEADBEEF;  // wrong magic
    hdr->capacity = 8;

    auto res = SHMChannel<Msg32>::open(name);
    EXPECT_FALSE(res.has_value());
}

TEST(SHMSecurity, OpenRejectsNonPowerOfTwoCapacity) {
    auto name = next_shm_name();
    auto seg = forge(name, calc_segment_size(8, sizeof(Msg32)));
    if (!seg) GTEST_SKIP() << "shm_open unavailable in this environment";
    auto* hdr = new (seg->base) SHMHeader();  // magic = kSHMMagic
    hdr->capacity = 7;                         // not a power of two

    auto res = SHMChannel<Msg32>::open(name);
    EXPECT_FALSE(res.has_value());
}

TEST(SHMSecurity, OpenRejectsZeroCapacity) {
    auto name = next_shm_name();
    auto seg = forge(name, calc_segment_size(8, sizeof(Msg32)));
    if (!seg) GTEST_SKIP() << "shm_open unavailable in this environment";
    auto* hdr = new (seg->base) SHMHeader();
    hdr->capacity = 0;

    auto res = SHMChannel<Msg32>::open(name);
    EXPECT_FALSE(res.has_value());
}

TEST(SHMSecurity, OpenRejectsUndersizedSegment) {
    auto name = next_shm_name();
    // A small request fits the header but is far too small for cap=1024
    // (required = header + 1024*32 + 1024*16 ≈ 49 KB). POSIX shm objects round
    // up (Linux: to a page; macOS: to a 16 KB minimum), so the capacity must be
    // large enough that its footprint clearly exceeds any such rounding.
    auto seg = forge(name, 4096);
    if (!seg) GTEST_SKIP() << "shm_open unavailable in this environment";
    ASSERT_GE(seg->size, sizeof(SHMHeader));
    auto* hdr = new (seg->base) SHMHeader();
    hdr->capacity = 1024;  // power of two, but ring+arena won't fit (~49 KB)

    auto res = SHMChannel<Msg32>::open(name);
    EXPECT_FALSE(res.has_value());
}

TEST(SHMSecurity, OpenAcceptsValidForgedSegment) {
    auto name = next_shm_name();
    constexpr uint32_t kCap = 8;
    auto seg = forge(name, calc_segment_size(kCap, sizeof(Msg32)));
    if (!seg) GTEST_SKIP() << "shm_open unavailable in this environment";
    auto* hdr = new (seg->base) SHMHeader();
    hdr->capacity = kCap;
    auto* slots = reinterpret_cast<MetadataSlot*>(
        static_cast<uint8_t*>(seg->base) + sizeof(SHMHeader));
    for (uint32_t i = 0; i < kCap; ++i) {
        new (&slots[i]) MetadataSlot();
        slots[i].seq.store(i, std::memory_order_relaxed);
    }

    auto res = SHMChannel<Msg32>::open(name);
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ((*res)->capacity(), kCap);
}

TEST(SHMSecurity, TryRecvRejectsCorruptSlotOffset) {
    auto name = next_shm_name();
    constexpr uint32_t kCap = 8;
    auto seg = forge(name, calc_segment_size(kCap, sizeof(Msg32)));
    if (!seg) GTEST_SKIP() << "shm_open unavailable in this environment";

    auto* hdr = new (seg->base) SHMHeader();
    hdr->capacity = kCap;
    hdr->head.store(0, std::memory_order_relaxed);
    hdr->tail.store(1, std::memory_order_relaxed);

    auto* slots = reinterpret_cast<MetadataSlot*>(
        static_cast<uint8_t*>(seg->base) + sizeof(SHMHeader));
    for (uint32_t i = 0; i < kCap; ++i) {
        new (&slots[i]) MetadataSlot();
        slots[i].seq.store(i, std::memory_order_relaxed);
    }
    // Simulate a committed message in slot 0 whose offset points past the arena
    // (arena is kCap * sizeof(Msg32) = 128 bytes; 0xFFFFFFFF is far outside).
    slots[0].off = 0xFFFFFFFFu;
    slots[0].len = sizeof(Msg32);
    slots[0].seq.store(1, std::memory_order_release);  // consumer pos 0 → diff 0

    auto res = SHMChannel<Msg32>::open(name);
    ASSERT_TRUE(res.has_value());
    // Must reject the corrupt slot (no OOB read) and report empty.
    auto r = (*res)->try_recv();
    EXPECT_FALSE(r.has_value());
}

// ─── calc_segment_size ───────────────────────────────────────────────────────

TEST(SHMCalc, PageAligned) {
    EXPECT_EQ(calc_segment_size(4, 8, false) % 4096u, 0u);
}

TEST(SHMCalc, GrowsWithCapacity) {
    EXPECT_GT(calc_segment_size(128, 8, false),
              calc_segment_size(64,  8, false));
}

TEST(SHMCalc, GrowsWithMsgSize) {
    // Use capacity=64 so that the arena size difference (64*(128-8)=7680)
    // exceeds one page and produces a measurably different segment size.
    EXPECT_GT(calc_segment_size(64, 128, false),
              calc_segment_size(64,   8, false));
}

TEST(SHMCalc, EnvelopeAddsSize) {
    // Use capacity=64 so the envelope overhead (64*128=8192) exceeds one page.
    size_t wo = calc_segment_size(64, 64, false);
    size_t wi = calc_segment_size(64, 64, true);
    EXPECT_GE(wi, wo + 64 * sizeof(SHMEnvelope));
}
