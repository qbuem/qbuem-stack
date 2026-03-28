/**
 * @file tests/pcie_layout_test.cpp
 * @brief v1.6.0: PCIe/VFIO, MSI-X Reactor, UDS Advanced API layout/basic tests.
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
    // BDF is PCIeDevice::BDF = std::string_view
    qbuem::pcie::PCIeDevice::BDF bdf = "0000:00:01.5";
    EXPECT_FALSE(bdf.empty());
    EXPECT_EQ(bdf, "0000:00:01.5");
}

TEST(PcieLayout, BDFToString) {
    qbuem::pcie::PCIeDevice::BDF bdf = "0000:03:00.1";
    // BDF is std::string_view — directly usable as string
    EXPECT_EQ(bdf, "0000:03:00.1");
    EXPECT_FALSE(bdf.empty());
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
    using namespace qbuem::uds;
    PeerCredentials pc{1234, 1000, 1000};
    EXPECT_EQ(pc.pid, 1234);
    EXPECT_EQ(pc.uid, 1000u);
    EXPECT_EQ(pc.gid, 1000u);
}

TEST(UdsAdvanced, RecvFdsResultFields) {
    using namespace qbuem::uds;
    RecvFdsResult r{};
    r.fd_count   = 0;
    r.data_bytes = 0;
    EXPECT_EQ(r.fd_count,   0u);
    EXPECT_EQ(r.data_bytes, 0u);
}

TEST(UdsAdvanced, SendFdsReturnTypeCheck) {
    // send_fds is overloaded (flat-buffer and scattered_span variants).
    // Disambiguate by assigning to the specific function pointer type; the
    // assignment is a compile-time proof that the overload with this exact
    // signature exists.
    using F1 = qbuem::Result<ssize_t>(*)(int, std::span<const int>, std::span<const uint8_t>) noexcept;
    using F2 = qbuem::Result<qbuem::uds::RecvFdsResult>(*)(int, std::span<int>, std::span<uint8_t>) noexcept;
    [[maybe_unused]] constexpr F1 send_fds_flat = &qbuem::uds::send_fds;
    EXPECT_NE(send_fds_flat, nullptr);
    EXPECT_TRUE((std::is_same_v<F2, decltype(&qbuem::uds::recv_fds)>));
}

TEST(UdsAdvanced, GetPeerCredentialsSignature) {
    using F = qbuem::Result<qbuem::uds::PeerCredentials>(*)(int) noexcept;
    EXPECT_TRUE((std::is_same_v<F, decltype(&qbuem::uds::get_peer_credentials)>));
}

TEST(UdsAdvanced, BindAbstractSignature) {
    // bind_abstract(string_view name, int type, int& listener) -> Result<void>
    using F = qbuem::Result<void>(*)(std::string_view, int, int&) noexcept;
    EXPECT_TRUE((std::is_same_v<F, decltype(&qbuem::uds::bind_abstract)>));
}

TEST(UdsAdvanced, ConnectAbstractSignature) {
    using F = qbuem::Result<int>(*)(std::string_view, int) noexcept;
    EXPECT_TRUE((std::is_same_v<F, decltype(&qbuem::uds::connect_abstract)>));
}
