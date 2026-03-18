#pragma once

/**
 * @file qbuem/xdp/xdp.hpp
 * @brief qbuem::xdp — AF_XDP eXpress Data Path umbrella header.
 * @ingroup qbuem_xdp
 *
 * Including this single header provides access to the full AF_XDP + UMEM feature set.
 *
 * ### Build Configuration
 * ```cmake
 * # CMakeLists.txt
 * find_package(qbuem-stack REQUIRED COMPONENTS xdp)
 * target_link_libraries(myapp PRIVATE qbuem-stack::xdp)
 * ```
 *
 * To enable AF_XDP, set the following CMake options:
 * ```sh
 * cmake -DQBUEM_XDP=ON [-DQBUEM_XDP_LIBBPF=ON] ..
 * ```
 *
 * - `QBUEM_XDP=ON`        : Defines `QBUEM_HAS_XDP`, enables AF_XDP headers
 * - `QBUEM_XDP_LIBBPF=ON` : Enables libbpf integration (xsk_umem__create and real implementations)
 *   Without this, the interface compiles but all functions return `errc::not_supported`
 *
 * ### Minimal Usage Example
 * ```cpp
 * #include <qbuem/xdp/xdp.hpp>
 *
 * // Create UMEM (NIC ↔ user-space shared memory)
 * auto umem = qbuem::xdp::Umem::create({
 *     .frame_count   = 4096,
 *     .frame_size    = 4096,
 *     .use_hugepages = true,
 * });
 *
 * // Create XSK socket (eth0, queue 0)
 * auto xsk = qbuem::xdp::XskSocket::create("eth0", 0, *umem, {
 *     .mode           = qbuem::xdp::XskConfig::Mode::Native,
 *     .force_zerocopy = true,
 * });
 *
 * // Prepare Fill Ring (supply empty frames for the kernel to write packets into)
 * umem->fill_frames(2048);
 *
 * // Receive loop (poll() / epoll() based)
 * qbuem::xdp::UmemFrame frames[64];
 * uint32_t n = xsk->recv(frames, 64);
 * for (uint32_t i = 0; i < n; ++i) {
 *     uint8_t* pkt = umem->data(frames[i]);
 *     // process packet ...
 * }
 * umem->fill_frames(n); // replenish consumed frames
 * ```
 *
 * ### Architecture Overview
 * ```
 *  ┌─────────────────────────────────────────────────────────┐
 *  │  qbuem::xdp                                             │
 *  │                                                         │
 *  │  Umem  ──── mmap ────►  shared memory (4096 * N bytes)  │
 *  │   │                           ▲                         │
 *  │   │  fill_ring (→ kernel)     │ DMA write (zero-copy)   │
 *  │   │  comp_ring (← kernel)     │                         │
 *  │   │                           │                         │
 *  │  XskSocket ◄─── NIC (native XDP hook) ◄─── Wire       │
 *  │   ├── rx_ring  (← kernel): receive-completed frames     │
 *  │   └── tx_ring  (→ kernel): transmit-request frames      │
 *  └─────────────────────────────────────────────────────────┘
 * ```
 *
 * ### Pipeline Integration Pattern
 * Pattern for forwarding packets received from XSK into a qbuem Pipeline:
 * ```cpp
 * // XDP receive → AsyncChannel → Pipeline Action
 * qbuem::AsyncChannel<PacketView> rx_channel{2048};
 *
 * // XDP polling coroutine
 * qbuem::Task<void> xdp_poll_loop(XskSocket& xsk, Umem& umem) {
 *     UmemFrame frames[64];
 *     while (true) {
 *         uint32_t n = xsk.recv(frames, 64);
 *         for (uint32_t i = 0; i < n; ++i) {
 *             co_await rx_channel.send({umem.data(frames[i]), frames[i].len});
 *         }
 *         umem.fill_frames(n);
 *         co_await qbuem::yield(); // yield to reactor
 *     }
 * }
 * ```
 *
 * ### References
 * - [Linux AF_XDP Documentation](https://www.kernel.org/doc/html/latest/networking/af_xdp.html)
 * - [libbpf GitHub](https://github.com/libbpf/libbpf)
 * - [xdp-tools / libxdp](https://github.com/xdp-project/xdp-tools)
 *
 * @note AF_XDP requires kernel 4.18+. For best performance (Native mode),
 *       XDP support in the NIC driver is required (mlx5, i40e, ixgbe, ice, nfp, etc.).
 *       SKB mode works on all NICs but has lower performance.
 *
 * @{
 */

#include <qbuem/xdp/umem.hpp>
#include <qbuem/xdp/xsk.hpp>

/** @} */ // end of qbuem_xdp
