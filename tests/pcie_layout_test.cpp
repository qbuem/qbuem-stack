/**
 * @file tests/pcie_layout_test.cpp
 * @brief v1.6.0: PCIe/VFIO, MSI-X Reactor, UDS Advanced API 레이아웃/기본 테스트.
 */

#include <gtest/gtest.h>

// ── v1.6.0 headers ──────────────────────────────────────────────────────────
#include <qbuem/pcie/pcie_device.hpp>
#include <qbuem/pcie/msix_reactor.hpp>
#include <qbuem/net/uds_advanced.hpp>

#include <cstdint>
#include <cstring>
#include <type_traits>

// ─── PCIeDevice / BDF ────────────────────────────────────────────────────────

TEST(PcieLayout, BDFConstruction) {
    using namespace qbuem::pcie;

    BDF bdf{0, 1, 5};
    EXPECT_EQ(bdf.bus,      0u);
    EXPECT_EQ(bdf.device,   1u);
    EXPECT_EQ(bdf.function, 5u);
}

TEST(PcieLayout, BDFToString) {
    using namespace qbuem::pcie;
    BDF bdf{0x03, 0x00, 0x01};
    // to_string() は "03:00.1" 형식을 반환해야 합니다
    auto s = bdf.to_string();
    EXPECT_FALSE(s.empty());
}

TEST(PcieLayout, BarMappingFields) {
    using namespace qbuem::pcie;
    BarMapping bm{};
    bm.vaddr   = nullptr;
    bm.size    = 4096;
    bm.bar_idx = 0;
    EXPECT_EQ(bm.size, 4096u);
    EXPECT_EQ(bm.bar_idx, 0u);
}

TEST(PcieLayout, DmaBufferFields) {
    using namespace qbuem::pcie;
    DmaBuffer db{};
    db.vaddr        = nullptr;
    db.iova         = 0xDEADBEEF00000000ULL;
    db.size         = 4096;
    db.container_fd = -1;
    EXPECT_EQ(db.iova, 0xDEADBEEF00000000ULL);
    EXPECT_EQ(db.size, 4096u);
    EXPECT_EQ(db.container_fd, -1);
}

TEST(PcieLayout, PCIeDeviceIsNotCopyable) {
    using T = qbuem::pcie::PCIeDevice;
    EXPECT_FALSE(std::is_copy_constructible_v<T>);
    EXPECT_FALSE(std::is_copy_assignable_v<T>);
}

// ─── MSIXReactor / VectorStats ───────────────────────────────────────────────

TEST(MsixLayout, VectorStatsAlignment) {
    using namespace qbuem::pcie;
    EXPECT_EQ(alignof(VectorStats), 64u);
    EXPECT_GE(sizeof(VectorStats), 24u); // 3 × atomic<uint64_t>
}

TEST(MsixLayout, VectorStatsZeroInit) {
    using namespace qbuem::pcie;
    VectorStats vs{};
    EXPECT_EQ(vs.irq_count.load(), 0u);
    EXPECT_EQ(vs.missed.load(),    0u);
    EXPECT_EQ(vs.latency_ns.load(),0u);
}

TEST(MsixLayout, MSIXReactorIsNotCopyable) {
    using T = qbuem::pcie::MSIXReactor;
    EXPECT_FALSE(std::is_copy_constructible_v<T>);
    EXPECT_FALSE(std::is_copy_assignable_v<T>);
}

// ─── UDS Advanced ────────────────────────────────────────────────────────────

TEST(UdsAdvanced, PeerCredentialsFields) {
    using namespace qbuem::net;
    PeerCredentials pc{1234, 1000, 1000};
    EXPECT_EQ(pc.pid, 1234);
    EXPECT_EQ(pc.uid, 1000u);
    EXPECT_EQ(pc.gid, 1000u);
}

TEST(UdsAdvanced, RecvFdsResultFields) {
    using namespace qbuem::net;
    RecvFdsResult r{};
    r.fd_count   = 0;
    r.data_bytes = 0;
    EXPECT_EQ(r.fd_count,   0u);
    EXPECT_EQ(r.data_bytes, 0u);
}

TEST(UdsAdvanced, SendFdsReturnTypeCheck) {
    // send_fds / recv_fds 는 qbuem::Result<ssize_t> / Result<RecvFdsResult> 반환
    using F1 = qbuem::Result<ssize_t>(*)(int, std::span<const int>, std::span<const uint8_t>);
    using F2 = qbuem::Result<qbuem::net::RecvFdsResult>(*)(int, std::span<int>, std::span<uint8_t>);
    EXPECT_TRUE((std::is_same_v<F1, decltype(&qbuem::net::send_fds)>));
    EXPECT_TRUE((std::is_same_v<F2, decltype(&qbuem::net::recv_fds)>));
}

TEST(UdsAdvanced, GetPeerCredentialsSignature) {
    using F = qbuem::Result<qbuem::net::PeerCredentials>(*)(int) noexcept;
    EXPECT_TRUE((std::is_same_v<F, decltype(&qbuem::net::get_peer_credentials)>));
}

TEST(UdsAdvanced, BindAbstractSignature) {
    // bind_abstract(string_view name, int type, int& listener) -> Result<void>
    using F = qbuem::Result<void>(*)(std::string_view, int, int&) noexcept;
    EXPECT_TRUE((std::is_same_v<F, decltype(&qbuem::net::bind_abstract)>));
}

TEST(UdsAdvanced, ConnectAbstractSignature) {
    using F = qbuem::Result<int>(*)(std::string_view, int) noexcept;
    EXPECT_TRUE((std::is_same_v<F, decltype(&qbuem::net::connect_abstract)>));
}
