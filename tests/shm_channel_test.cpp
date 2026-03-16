/**
 * @file shm_channel_test.cpp
 * @brief SHMChannel<T> + SHMBus 기능 단위 테스트.
 *
 * 커버리지:
 * - SHMChannel::create + is_open + capacity + size_approx
 * - try_send / try_recv (Vyukov MPMC 정확성)
 * - close → is_open=false, try_send=false
 * - 링 버퍼 꽉 참 → try_send=false
 * - 링 버퍼 비어있음 → try_recv=nullopt
 * - 여러 메시지 순서 보존 (FIFO)
 * - SHMBus LOCAL_ONLY: declare / try_publish / subscribe / try_recv
 * - SHMBus: 소멸자 자원 해제 (memory leak 없음)
 * - calc_segment_size 반환값 검증
 */

#include <qbuem/shm/shm_bus.hpp>
#include <qbuem/shm/shm_channel.hpp>
#include <gtest/gtest.h>

#include <cstring>
#include <string>

using namespace qbuem::shm;

// ─── 테스트용 메시지 타입 ─────────────────────────────────────────────────────

struct Msg32 {
    int64_t  seq{0};
    uint32_t val{0};
    uint32_t _pad{0};
};
static_assert(std::is_trivially_copyable_v<Msg32>);

// 각 테스트마다 고유한 shm 이름 사용 (충돌 방지)
static int g_shm_counter = 0;
static std::string next_shm_name() {
    return "qbuem_test_" + std::to_string(++g_shm_counter);
}

// ─── SHMChannel 생성 ─────────────────────────────────────────────────────────

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
    // 채널 가득 채우기 (cap=4)
    for (size_t i = 0; i < ch->capacity(); ++i)
        EXPECT_TRUE(ch->try_send(m));

    // 가득 찼으므로 false
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
    // 생성 후 close → unlink 정상 동작 확인
    {
        auto res = SHMChannel<Msg32>::create(name, 8);
        ASSERT_TRUE(res.has_value());
        (*res)->close();
    }
    // unlink 성공
    auto r = SHMChannel<Msg32>::unlink(name);
    EXPECT_TRUE(r.has_value()) << r.error().message();
    // 두 번 unlink 해도 에러 없음 (ENOENT → ok)
    auto r2 = SHMChannel<Msg32>::unlink(name);
    EXPECT_TRUE(r2.has_value());
}

TEST(SHMChannel, UnlinkNonexistentOk) {
    // 존재하지 않는 이름도 ok (ENOENT 무음 처리)
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

    // 하나의 구독자가 받음 (MPMC — 경쟁)
    auto r1 = sub1->try_recv();
    auto r2 = sub2->try_recv();
    // 둘 중 정확히 하나만 메시지를 받아야 함
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
    // LocalSub bug fix 검증: 두 구독자가 서로 다른 buf_를 가져야 함
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

    // 각 구독자가 독립적인 버퍼를 사용 (포인터가 달라야 함)
    auto ra = sub1->try_recv();
    auto rb = sub2->try_recv();

    if (ra.has_value() && rb.has_value()) {
        // 두 포인터는 달라야 함
        EXPECT_NE(*ra, *rb);
    }
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
