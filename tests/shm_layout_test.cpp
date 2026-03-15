/**
 * @file shm_layout_test.cpp
 * @brief SHM 메시지 인프라 레이아웃 및 구조체 제약 단위 테스트.
 *
 * 커버리지:
 * - SHMHeader 크기(64B) 및 정렬(64B) 검증
 * - MetadataSlot 크기(32B) 및 정렬(32B) 검증
 * - SHMEnvelope 크기(128B) 검증
 * - SHMMagic 상수 검증
 * - calc_segment_size() 반환값이 페이지 경계 이상임을 확인
 * - TopicDescriptor 이름 설정/조회
 */

#include <qbuem/shm/shm_channel.hpp>
#include <qbuem/shm/shm_bus.hpp>
#include <gtest/gtest.h>

#include <cstring>

using namespace qbuem::shm;

// ─── 구조체 크기/정렬 제약 ────────────────────────────────────────────────────

TEST(SHMLayout, HeaderSize) {
    EXPECT_EQ(sizeof(SHMHeader), 64u);
}

TEST(SHMLayout, HeaderAlignment) {
    EXPECT_EQ(alignof(SHMHeader), 64u);
}

TEST(SHMLayout, MetadataSlotSize) {
    EXPECT_EQ(sizeof(MetadataSlot), 32u);
}

TEST(SHMLayout, MetadataSlotAlignment) {
    EXPECT_EQ(alignof(MetadataSlot), 32u);
}

TEST(SHMLayout, EnvelopeSize) {
    EXPECT_EQ(sizeof(SHMEnvelope), 128u);
}

TEST(SHMLayout, MagicConstant) {
    // "QBUM" in little-endian: Q=0x51, B=0x42, U=0x55, M=0x4D
    EXPECT_EQ(kSHMMagic, 0x5142554Du);
}

// ─── 헤더 초기 상태 ──────────────────────────────────────────────────────────

TEST(SHMLayout, HeaderDefaultState) {
    SHMHeader hdr;
    EXPECT_EQ(hdr.tail.load(), 0u);
    EXPECT_EQ(hdr.head.load(), 0u);
    EXPECT_EQ(hdr.magic, kSHMMagic);
    // state bit0 = Active
    EXPECT_EQ(hdr.state.load() & 1u, 1u);
}

// ─── MetadataSlot 초기 상태 ──────────────────────────────────────────────────

TEST(SHMLayout, MetadataSlotDefault) {
    MetadataSlot slot;
    EXPECT_EQ(slot.seq.load(), 0u);
    EXPECT_EQ(slot.off, 0u);
    EXPECT_EQ(slot.len, 0u);
    EXPECT_EQ(slot.tid, 0u);
    EXPECT_EQ(slot.flg, 0u);
    EXPECT_EQ(slot.epc, 0u);
}

// ─── calc_segment_size ───────────────────────────────────────────────────────

TEST(SHMLayout, CalcSegmentSizeMinPage) {
    // 아무리 작아도 반환값은 페이지 크기(4096B) 이상이어야 합니다
    size_t sz = calc_segment_size(4, 8, false);
    EXPECT_GE(sz, 4096u);
}

TEST(SHMLayout, CalcSegmentSizeGrowsWithCapacity) {
    size_t s1 = calc_segment_size(64,  64, false);
    size_t s2 = calc_segment_size(128, 64, false);
    EXPECT_GT(s2, s1);
}

TEST(SHMLayout, CalcSegmentSizeEnvelopeAdds128) {
    size_t without = calc_segment_size(16, 64, false);
    size_t with_   = calc_segment_size(16, 64, true);
    // envelope가 붙으면 capacity * 128B 이상 커져야 합니다
    EXPECT_GE(with_, without + 16 * 128u);
}

// ─── TopicDescriptor ─────────────────────────────────────────────────────────

TEST(SHMLayout, TopicDescriptorName) {
    TopicDescriptor desc;
    desc.set_name("trading.orders");
    EXPECT_EQ(desc.get_name(), "trading.orders");
}

TEST(SHMLayout, TopicDescriptorNameTruncation) {
    // kMaxNameLen = 63 — 그보다 긴 이름은 잘립니다
    std::string long_name(100, 'x');
    TopicDescriptor desc;
    desc.set_name(long_name);
    EXPECT_EQ(desc.get_name().size(), 63u);
}

TEST(SHMLayout, TopicDescriptorScopeDefault) {
    TopicDescriptor desc;
    EXPECT_EQ(desc.scope, TopicScope::LOCAL_ONLY);
}

// ─── SHMBus 토픽 레지스트리 (로컬 스코프) ────────────────────────────────────

struct TestMsg { int value; };

TEST(SHMBus, DeclareAndHasTopic) {
    SHMBus bus;
    EXPECT_FALSE(bus.has_topic("test.topic"));
    bool ok = bus.declare<TestMsg>("test.topic", TopicScope::LOCAL_ONLY, 64);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(bus.has_topic("test.topic"));
    EXPECT_EQ(bus.topic_count(), 1u);
}

TEST(SHMBus, DuplicateDeclareReturnsFalse) {
    SHMBus bus;
    bus.declare<TestMsg>("dup.topic", TopicScope::LOCAL_ONLY, 64);
    EXPECT_FALSE(bus.declare<TestMsg>("dup.topic", TopicScope::LOCAL_ONLY, 64));
    EXPECT_EQ(bus.topic_count(), 1u);
}

TEST(SHMBus, SubscribeUnknownTopicReturnsNull) {
    SHMBus bus;
    auto sub = bus.subscribe<TestMsg>("nonexistent");
    EXPECT_EQ(sub, nullptr);
}

TEST(SHMBus, TryPublishUnknownTopicReturnsFalse) {
    SHMBus bus;
    TestMsg msg{42};
    EXPECT_FALSE(bus.try_publish("unknown", msg));
}

TEST(SHMBus, TryPublishTypeMismatchReturnsFalse) {
    SHMBus bus;
    bus.declare<TestMsg>("mismatch.topic", TopicScope::LOCAL_ONLY, 64);
    // 다른 타입으로 발행 시도
    struct OtherMsg { double x; };
    OtherMsg other{1.0};
    EXPECT_FALSE(bus.try_publish("mismatch.topic", other));
}
