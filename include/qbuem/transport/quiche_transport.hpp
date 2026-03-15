#pragma once

/**
 * @file qbuem/transport/quiche_transport.hpp
 * @brief QUIC/HTTP3 전송 레이어 — quiche FFI ITransport 레퍼런스 구현 가이드.
 * @ingroup qbuem_transport
 *
 * ## 개요
 *
 * 이 파일은 `ITransport` 인터페이스를 이용해 QUIC/HTTP3 전송 계층을
 * 서비스에서 직접 구현하는 방법을 안내하는 **레퍼런스 가이드**입니다.
 *
 * qbuem-stack은 zero-dependency 원칙에 따라 quiche/ngtcp2를 직접 링크하지
 * 않습니다. QUIC 기능이 필요한 서비스는 아래 가이드에 따라 자체 구현체를
 * `ITransport`로 주입하면 됩니다.
 *
 * ## 의존성 선택
 *
 * | 라이브러리 | 특징 | 추천 상황 |
 * |-----------|------|----------|
 * | **quiche** (Cloudflare) | Rust 기반, 안정적, HTTP/3 내장 | 프로덕션 권장 |
 * | **ngtcp2** | C 기반, 저수준 제어 | 커스텀 프로토콜 |
 * | **msquic** (Microsoft) | Windows/Linux, 고성능 | Microsoft 스택 연동 |
 * | **mvfst** (Meta) | C++, folly 기반 | Meta 인프라 스택 |
 *
 * ## 구현 예시 (quiche 기반)
 *
 * ```cmake
 * # CMakeLists.txt (서비스 프로젝트)
 * find_package(qbuem-stack REQUIRED COMPONENTS transport)
 *
 * # quiche는 cargo build로 생성된 정적 라이브러리를 링크
 * add_library(quiche_transport_impl STATIC quiche_transport.cpp)
 * target_link_libraries(quiche_transport_impl
 *     PUBLIC  qbuem-stack::transport
 *     PRIVATE quiche  # cargo build --release → target/release/libquiche.a
 * )
 * ```
 *
 * ```cpp
 * // quiche_transport.hpp (서비스 구현)
 * #include <qbuem/transport/quiche_transport.hpp>  // 이 가이드 헤더
 *
 * class QuicheTransport : public qbuem::ITransport {
 * public:
 *     explicit QuicheTransport(quiche_conn* conn) : conn_(conn) {}
 *
 *     qbuem::Task<qbuem::Result<size_t>>
 *     read(qbuem::MutableBufferView buf) override {
 *         // QUIC stream에서 읽기 (스트림 ID는 컨텍스트로 전달)
 *         ssize_t n = quiche_conn_stream_recv(conn_, stream_id_,
 *             reinterpret_cast<uint8_t*>(buf.data()), buf.size(), &fin_);
 *         if (n == QUICHE_ERR_DONE) {
 *             // 데이터 없음 → reactor wakeup 대기 (UDP recv 후 재시도)
 *             co_await io_event_;
 *             co_return co_await read(buf); // 재귀 호출
 *         }
 *         if (n < 0) {
 *             co_return qbuem::unexpected(
 *                 std::make_error_code(std::errc::io_error));
 *         }
 *         co_return static_cast<size_t>(n);
 *     }
 *
 *     qbuem::Task<qbuem::Result<size_t>>
 *     write(qbuem::BufferView buf) override {
 *         ssize_t n = quiche_conn_stream_send(conn_, stream_id_,
 *             reinterpret_cast<const uint8_t*>(buf.data()), buf.size(),
 *             /*fin=*/false);
 *         if (n < 0) {
 *             co_return qbuem::unexpected(
 *                 std::make_error_code(std::errc::io_error));
 *         }
 *         co_return static_cast<size_t>(n);
 *     }
 *
 *     qbuem::Task<void> close() override {
 *         quiche_conn_close(conn_, /*app=*/true, 0, nullptr, 0);
 *         co_return;
 *     }
 *
 *     [[nodiscard]] bool is_open() const noexcept override {
 *         return !quiche_conn_is_closed(conn_);
 *     }
 *
 * private:
 *     quiche_conn* conn_;
 *     uint64_t     stream_id_ = 0;
 *     bool         fin_       = false;
 *     // reactor wakeup awaiter (UDP recv 완료 신호)
 *     qbuem::IoEvent io_event_;
 * };
 * ```
 *
 * ## HTTP/3 요청 처리 흐름
 *
 * ```
 * UDP Socket (qbuem::UdpSocket)
 *     │
 *     │  recvfrom() → UDP 데이터그램
 *     ▼
 * quiche_conn_recv()        ← QUIC 패킷 처리 (암호화 해제, ACK 등)
 *     │
 *     │  quiche_h3_conn_poll()
 *     ▼
 * HTTP/3 이벤트 디스패치
 *     ├── H3_EVENT_HEADERS  → Router 매핑 → Handler 호출
 *     ├── H3_EVENT_DATA     → request body 스트리밍
 *     └── H3_EVENT_FINISHED → 요청 완료
 *
 * Handler 응답 → quiche_h3_send_response()
 *     │
 *     │  quiche_conn_send() → UDP 데이터그램
 *     ▼
 * UDP Socket.sendto()
 * ```
 *
 * ## 0-RTT (Zero Round Trip Time)
 *
 * ```cpp
 * // 서버: 0-RTT session ticket 설정
 * quiche_config_set_max_idle_timeout(cfg, 30000);   // 30초
 * quiche_config_set_initial_max_data(cfg, 10485760); // 10 MiB
 *
 * // 클라이언트: early data 전송
 * quiche_conn_send_ach(conn, buf, len);  // 0-RTT QUIC 패킷
 * // HTTP/3 요청을 TLS 핸드셰이크 완료 전에 송신 가능
 * ```
 *
 * ## Connection Migration (연결 마이그레이션)
 *
 * QUIC은 IP 주소/포트 변경 없이 연결을 유지합니다 (모바일 네트워크 전환 등).
 *
 * ```cpp
 * // 연결 이동 후 새 경로 검증
 * quiche_conn_probe_path(conn, new_local, local_len, new_peer, peer_len, &seq);
 * // 검증 완료 후 경로 전환
 * quiche_conn_migrate(conn, new_local, local_len, new_peer, peer_len);
 * ```
 *
 * ## 멀티플렉싱 (HOL Blocking 없음)
 *
 * ```cpp
 * // HTTP/3는 스트림 단위 멀티플렉싱 → Head-of-Line Blocking 없음
 * // qbuem Pipeline과 연결:
 * qbuem::AsyncChannel<Http3Request> req_channel{256};
 *
 * // 각 QUIC 스트림마다 독립 Task
 * for (auto stream_id : new_streams) {
 *     dispatcher.spawn(handle_h3_stream(conn, stream_id, req_channel));
 * }
 * ```
 *
 * ## 빌드 가이드 (quiche)
 *
 * ```sh
 * # 1. quiche 빌드 (Rust 필요)
 * git clone --recursive https://github.com/cloudflare/quiche.git
 * cd quiche && cargo build --release --features ffi
 *
 * # 2. 서비스 빌드
 * cmake -DQUICHE_DIR=/path/to/quiche/target/release ..
 * ```
 *
 * ## 참고 자료
 *
 * - [quiche GitHub](https://github.com/cloudflare/quiche)
 * - [RFC 9000 — QUIC](https://www.rfc-editor.org/rfc/rfc9000)
 * - [RFC 9114 — HTTP/3](https://www.rfc-editor.org/rfc/rfc9114)
 * - [RFC 9001 — TLS 1.3 for QUIC](https://www.rfc-editor.org/rfc/rfc9001)
 * - [QUIC 성능 측정 (IETF)](https://datatracker.ietf.org/doc/html/rfc9312)
 *
 * @note 이 헤더는 구현 가이드이며 실제 코드를 포함하지 않습니다.
 *       quiche, ngtcp2 등의 라이브러리는 서비스에서 직접 링크하세요.
 */

#include <qbuem/core/transport.hpp>
#include <qbuem/net/udp_socket.hpp>

namespace qbuem {

/**
 * @brief QUIC/HTTP3 ITransport 구현 템플릿.
 *
 * 서비스에서 이 구조체를 참고하여 quiche / ngtcp2 등의
 * QUIC 라이브러리를 `ITransport`로 감싸는 구현체를 작성하세요.
 *
 * @par 최소 구현 목록
 * - `read(MutableBufferView)` — QUIC 스트림에서 데이터 읽기
 * - `write(BufferView)` — QUIC 스트림으로 데이터 쓰기
 * - `close()` — 스트림 / 연결 종료
 * - `is_open()` — 연결 활성 여부
 *
 * @par QUIC 특유 추가 구현
 * - 0-RTT early data 처리
 * - Connection migration 처리
 * - ALPN 협상 ("h3" 또는 커스텀 프로토콜)
 * - DATAGRAM 프레임 (RFC 9221, unreliable messaging)
 */
struct QuicTransportGuide {
    // 이 구조체는 문서화 목적으로만 존재합니다.
    // 실제 구현은 서비스 코드에서 ITransport를 상속받아 작성하세요.
    static_assert(sizeof(QuicTransportGuide) == 0,
        "QuicTransportGuide is a documentation-only type. "
        "Implement ITransport in your service code.");
};

} // namespace qbuem
