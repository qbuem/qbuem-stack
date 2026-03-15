/**
 * @file tests/spdk_layout_test.cpp
 * @brief v1.7.0: RDMA Channel, eBPF Tracer, NVMe IO 레이아웃/기본 테스트.
 */

#include <gtest/gtest.h>

// ── v1.7.0 headers ──────────────────────────────────────────────────────────
#include <qbuem/rdma/rdma_channel.hpp>
#include <qbuem/ebpf/ebpf_tracer.hpp>
#include <qbuem/spdk/nvme_io.hpp>

#include <cstring>
#include <type_traits>

// ─── RDMA ────────────────────────────────────────────────────────────────────

TEST(RdmaLayout, QPInfoFields) {
    using namespace qbuem::rdma;
    QPInfo qi{};
    qi.qp_num = 42;
    qi.lid    = 0x0001;
    qi.psn    = 0x12345;
    EXPECT_EQ(qi.qp_num, 42u);
    EXPECT_EQ(qi.lid,    1u);
    EXPECT_EQ(qi.psn,    0x12345u);
    // GID: 16 bytes
    EXPECT_EQ(sizeof(qi.gid), 16u);
}

TEST(RdmaLayout, CompletionFields) {
    using namespace qbuem::rdma;
    Completion c{};
    c.wr_id    = 99;
    c.bytes    = 4096;
    c.status   = 0;
    c.opcode   = 0;
    EXPECT_EQ(c.wr_id,  99u);
    EXPECT_EQ(c.bytes,  4096u);

    // ok() should be true when status == 0
    EXPECT_TRUE(c.ok());
    c.status = 1;
    EXPECT_FALSE(c.ok());
}

TEST(RdmaLayout, RDMAContextIsNotCopyable) {
    using T = qbuem::rdma::RDMAContext;
    EXPECT_FALSE(std::is_copy_constructible_v<T>);
    EXPECT_FALSE(std::is_copy_assignable_v<T>);
}

TEST(RdmaLayout, RDMAChannelIsNotCopyable) {
    using T = qbuem::rdma::RDMAChannel;
    EXPECT_FALSE(std::is_copy_constructible_v<T>);
    EXPECT_FALSE(std::is_copy_assignable_v<T>);
}

// ─── eBPF Tracer ─────────────────────────────────────────────────────────────

TEST(EbpfLayout, TraceEventSize) {
    using namespace qbuem::ebpf;
    // TraceEvent는 정확히 64바이트 (캐시 라인)
    static_assert(sizeof(TraceEvent) == 64, "TraceEvent must be 64 bytes");
    EXPECT_EQ(sizeof(TraceEvent), 64u);
}

TEST(EbpfLayout, TraceEventAlignment) {
    using namespace qbuem::ebpf;
    EXPECT_EQ(alignof(TraceEvent), 64u);
}

TEST(EbpfLayout, TraceEventLabelSetGet) {
    using namespace qbuem::ebpf;
    TraceEvent evt{};
    evt.set_label("pipeline.http.parse");
    auto lbl = evt.get_label();
    EXPECT_EQ(lbl, "pipeline.http.parse");
}

TEST(EbpfLayout, TraceEventLabelTruncation) {
    using namespace qbuem::ebpf;
    TraceEvent evt{};
    // 23자 초과 레이블은 잘려야 함
    evt.set_label("this_label_is_over_23_characters_long");
    auto lbl = evt.get_label();
    EXPECT_EQ(lbl.size(), 23u);
}

TEST(EbpfLayout, EventTypeValues) {
    using namespace qbuem::ebpf;
    EXPECT_EQ(static_cast<uint16_t>(EventType::TcpAccept),           1u);
    EXPECT_EQ(static_cast<uint16_t>(EventType::HttpParseBegin),      3u);
    EXPECT_EQ(static_cast<uint16_t>(EventType::PipelineActionEnter), 5u);
    EXPECT_EQ(static_cast<uint16_t>(EventType::JwtVerify),          12u);
    EXPECT_EQ(static_cast<uint16_t>(EventType::Custom),            255u);
}

TEST(EbpfLayout, BPFStatsFields) {
    using namespace qbuem::ebpf;
    BPFStats stats{};
    EXPECT_EQ(stats.tcp_accepts,       0u);
    EXPECT_EQ(stats.http_requests,     0u);
    EXPECT_EQ(stats.jwt_verifications, 0u);
}

TEST(EbpfLayout, TracePointMacro) {
    // QBUEM_TRACE은 no-op 함수 호출이어야 함 — 컴파일 성공만 확인
    QBUEM_TRACE("test.event", 1u, 2u);
    SUCCEED();
}

TEST(EbpfLayout, EBPFTracerIsNotCopyable) {
    using T = qbuem::ebpf::EBPFTracer;
    EXPECT_FALSE(std::is_copy_constructible_v<T>);
    EXPECT_FALSE(std::is_copy_assignable_v<T>);
}

// ─── NVMe IO ─────────────────────────────────────────────────────────────────

TEST(NvmeLayout, NVMeResultOk) {
    using namespace qbuem::spdk;
    NVMeResult r{};
    r.status = 0;
    EXPECT_TRUE(r.ok());

    // status bit1 이상 = 오류
    r.status = 0x02;
    EXPECT_FALSE(r.ok());
}

TEST(NvmeLayout, NVMeResultSCT) {
    using namespace qbuem::spdk;
    NVMeResult r{};
    // bits [11:9] = SCT
    r.status = (0x1u << 9);
    EXPECT_EQ(r.sct(), 1u);
}

TEST(NvmeLayout, NVMeResultSC) {
    using namespace qbuem::spdk;
    NVMeResult r{};
    // bits [8:1] = SC
    r.status = (0x05u << 1);
    EXPECT_EQ(r.sc(), 5u);
}

TEST(NvmeLayout, NVMeDeviceInfoTotalBytes) {
    using namespace qbuem::spdk;
    NVMeDeviceInfo info{};
    info.ns_size  = 1024;
    info.lba_size = 4096;
    EXPECT_EQ(info.total_bytes(), 1024u * 4096u);
}

TEST(NvmeLayout, NVMeDeviceInfoDefaults) {
    using namespace qbuem::spdk;
    NVMeDeviceInfo info{};
    EXPECT_EQ(info.lba_size, 512u);  // 기본값
    EXPECT_EQ(info.ns_id,    1u);
    EXPECT_FALSE(info.volatile_write_cache);
}

TEST(NvmeLayout, NVMeStatsZeroInit) {
    using namespace qbuem::spdk;
    NVMeStats stats{};
    EXPECT_EQ(stats.read_ops.load(),  0u);
    EXPECT_EQ(stats.write_ops.load(), 0u);
    EXPECT_EQ(stats.errors.load(),    0u);
}

TEST(NvmeLayout, NVMeIOContextIsNotCopyable) {
    using T = qbuem::spdk::NVMeIOContext;
    EXPECT_FALSE(std::is_copy_constructible_v<T>);
    EXPECT_FALSE(std::is_copy_assignable_v<T>);
}
