#pragma once

/**
 * @file qbuem/xdp/xdp.hpp
 * @brief qbuem::xdp — AF_XDP eXpress Data Path 통합 헤더.
 * @ingroup qbuem_xdp
 *
 * 이 헤더 하나로 AF_XDP + UMEM 기능 전체를 사용할 수 있습니다.
 *
 * ### 빌드 설정
 * ```cmake
 * # CMakeLists.txt
 * find_package(qbuem-stack REQUIRED COMPONENTS xdp)
 * target_link_libraries(myapp PRIVATE qbuem-stack::xdp)
 * ```
 *
 * AF_XDP를 활성화하려면 CMake 옵션을 설정하세요:
 * ```sh
 * cmake -DQBUEM_XDP=ON [-DQBUEM_XDP_LIBBPF=ON] ..
 * ```
 *
 * - `QBUEM_XDP=ON`        : `QBUEM_HAS_XDP` 정의, AF_XDP 헤더 사용 가능
 * - `QBUEM_XDP_LIBBPF=ON` : libbpf 연동 활성화 (xsk_umem__create 등 실제 구현)
 *   없으면 인터페이스는 컴파일되나 모든 함수가 `errc::not_supported` 반환
 *
 * ### 최소 사용 예시
 * ```cpp
 * #include <qbuem/xdp/xdp.hpp>
 *
 * // UMEM 생성 (NIC ↔ 유저스페이스 공유 메모리)
 * auto umem = qbuem::xdp::Umem::create({
 *     .frame_count   = 4096,
 *     .frame_size    = 4096,
 *     .use_hugepages = true,
 * });
 *
 * // XSK 소켓 생성 (eth0, 큐 0)
 * auto xsk = qbuem::xdp::XskSocket::create("eth0", 0, *umem, {
 *     .mode           = qbuem::xdp::XskConfig::Mode::Native,
 *     .force_zerocopy = true,
 * });
 *
 * // Fill Ring 준비 (커널이 패킷을 기록할 빈 프레임 공급)
 * umem->fill_frames(2048);
 *
 * // 수신 루프 (poll() / epoll() 기반)
 * qbuem::xdp::UmemFrame frames[64];
 * uint32_t n = xsk->recv(frames, 64);
 * for (uint32_t i = 0; i < n; ++i) {
 *     uint8_t* pkt = umem->data(frames[i]);
 *     // 패킷 처리 ...
 * }
 * umem->fill_frames(n); // 소비한 만큼 재공급
 * ```
 *
 * ### 아키텍처 개요
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
 *  │   ├── rx_ring  (← kernel): 수신 완료 프레임              │
 *  │   └── tx_ring  (→ kernel): 전송 요청 프레임              │
 *  └─────────────────────────────────────────────────────────┘
 * ```
 *
 * ### Pipeline 통합 패턴
 * XSK에서 받은 패킷을 qbuem Pipeline으로 보내는 패턴:
 * ```cpp
 * // XDP 수신 → AsyncChannel → Pipeline Action
 * qbuem::AsyncChannel<PacketView> rx_channel{2048};
 *
 * // XDP 폴링 코루틴
 * qbuem::Task<void> xdp_poll_loop(XskSocket& xsk, Umem& umem) {
 *     UmemFrame frames[64];
 *     while (true) {
 *         uint32_t n = xsk.recv(frames, 64);
 *         for (uint32_t i = 0; i < n; ++i) {
 *             co_await rx_channel.send({umem.data(frames[i]), frames[i].len});
 *         }
 *         umem.fill_frames(n);
 *         co_await qbuem::yield(); // reactor 양보
 *     }
 * }
 * ```
 *
 * ### 참고 자료
 * - [Linux AF_XDP Documentation](https://www.kernel.org/doc/html/latest/networking/af_xdp.html)
 * - [libbpf GitHub](https://github.com/libbpf/libbpf)
 * - [xdp-tools / libxdp](https://github.com/xdp-project/xdp-tools)
 *
 * @note AF_XDP는 커널 4.18+이 필요하며, 최고 성능(Native mode)을 위해서는
 *       NIC 드라이버의 XDP 지원이 필수입니다 (mlx5, i40e, ixgbe, ice, nfp 등).
 *       SKB mode는 모든 NIC에서 동작하나 성능이 낮습니다.
 *
 * @{
 */

#include <qbuem/xdp/umem.hpp>
#include <qbuem/xdp/xsk.hpp>

/** @} */ // end of qbuem_xdp
