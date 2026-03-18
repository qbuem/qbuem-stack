/**
 * @file shm_layout_test.cpp
 * @brief SHM message infrastructure layout and struct constraint unit tests.
 *
 * Coverage:
 * - SHMHeader size (64B) and alignment (64B) validation
 * - MetadataSlot size (32B) and alignment (32B) validation
 * - SHMEnvelope size (128B) validation
 * - SHMMagic constant validation
 * - Verify calc_segment_size() return value is at least a page boundary
 * - TopicDescriptor name set/get
 */

#include <qbuem/shm/shm_channel.hpp>
#include <qbuem/shm/shm_bus.hpp>
#include <gtest/gtest.h>

#include <cstring>

using namespace qbuem::shm;

// ─── Struct size/alignment constraints ───────────────────────────────────────

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

// ─── Header initial state ────────────────────────────────────────────────────

TEST(SHMLayout, HeaderDefaultState) {
    SHMHeader hdr;
    EXPECT_EQ(hdr.tail.load(), 0u);
    EXPECT_EQ(hdr.head.load(), 0u);
    EXPECT_EQ(hdr.magic, kSHMMagic);
    // state bit0 = Active
    EXPECT_EQ(hdr.state.load() & 1u, 1u);
}

// ─── MetadataSlot initial state ──────────────────────────────────────────────

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
    // Return value must always be at least the page size (4096B) regardless of inputs
    size_t sz = calc_segment_size(4, 8, false);
    EXPECT_GE(sz, 4096u);
}

TEST(SHMLayout, CalcSegmentSizeGrowsWithCapacity) {
    size_t s1 = calc_segment_size(64,  64, false);
    size_t s2 = calc_segment_size(128, 64, false);
    EXPECT_GT(s2, s1);
}

TEST(SHMLayout, CalcSegmentSizeEnvelopeAdds128) {
    // Use capacity=64 so the envelope overhead (64*128=8192) exceeds one page.
    size_t without = calc_segment_size(64, 64, false);
    size_t with_   = calc_segment_size(64, 64, true);
    // With envelope, size must grow by at least capacity * 128B
    EXPECT_GE(with_, without + 64 * 128u);
}

// ─── TopicDescriptor ─────────────────────────────────────────────────────────

TEST(SHMLayout, TopicDescriptorName) {
    TopicDescriptor desc;
    desc.set_name("trading.orders");
    EXPECT_EQ(desc.get_name(), "trading.orders");
}

TEST(SHMLayout, TopicDescriptorNameTruncation) {
    // kMaxNameLen = 63 — names longer than that are truncated
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
