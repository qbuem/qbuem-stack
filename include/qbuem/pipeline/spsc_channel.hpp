#pragma once

/**
 * @file qbuem/pipeline/spsc_channel.hpp
 * @brief 락-프리 SPSC 채널 — SpscChannel
 * @defgroup qbuem_spsc_channel SpscChannel
 * @ingroup qbuem_pipeline
 *
 * 단일 생산자/단일 소비자(SPSC) 락-프리 링 버퍼 채널입니다.
 * Lamport queue 알고리즘을 기반으로 head_/tail_을 별도 캐시 라인에 배치합니다.
 * 1:1 시나리오에서 MPMC AsyncChannel보다 빠릅니다.
 *
 * ## 구현 특성
 * - `head_` (소비자)와 `tail_` (생산자)를 `alignas(64)`로 분리
 * - 용량은 자동으로 2의 거듭제곱으로 올림 처리
 * - 비동기 대기: `Reactor::post`를 통한 크로스-리액터 wake
 *
 * ## 사용 예시
 * @code
 * SpscChannel<int> chan(1024);
 * chan.try_send(42);          // 생산자 스레드
 * auto v = chan.try_recv();   // 소비자 스레드
 * @endcode
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/reactor.hpp>
#include <qbuem/core/task.hpp>

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace qbuem {

/**
 * @brief 락-프리 SPSC (단일 생산자/단일 소비자) 채널.
 *
 * 용량은 생성 시 자동으로 2의 거듭제곱으로 올림 처리됩니다.
 *
 * @tparam T 전송할 값의 타입 (이동 가능해야 함).
 */
template <typename T>
class SpscChannel {
public:
    /**
     * @brief 지정한 용량으로 채널을 생성합니다.
     *
     * 용량은 자동으로 2의 거듭제곱으로 올림 처리됩니다.
     * (예: 5 → 8, 9 → 16)
     *
     * @param capacity 희망 용량 (2의 거듭제곱으로 올림).
     */
    explicit SpscChannel(size_t capacity) {
        size_t cap = next_pow2(std::max(capacity, size_t{2}));
        mask_ = cap - 1;
        buffer_.resize(cap);
    }

    SpscChannel(const SpscChannel&) = delete;
    SpscChannel& operator=(const SpscChannel&) = delete;

    // -------------------------------------------------------------------------
    // 논블로킹 try_send / try_recv
    // -------------------------------------------------------------------------

    /**
     * @brief 아이템을 채널에 넣으려 시도합니다 (논블로킹, 생산자 스레드).
     *
     * @param item 전송할 아이템 (const 참조).
     * @returns 성공이면 true, 채널이 가득 차거나 닫혔으면 false.
     */
    bool try_send(const T& item) {
        return try_send_impl(T(item));
    }

    /**
     * @brief 아이템을 채널에 넣으려 시도합니다 (논블로킹, 생산자 스레드).
     *
     * @param item 전송할 아이템 (이동).
     * @returns 성공이면 true, 채널이 가득 차거나 닫혔으면 false.
     */
    bool try_send(T&& item) {
        return try_send_impl(std::move(item));
    }

    /**
     * @brief 채널에서 아이템을 꺼내려 시도합니다 (논블로킹, 소비자 스레드).
     *
     * @returns 아이템이 있으면 `std::optional<T>`, 없으면 `std::nullopt`.
     */
    std::optional<T> try_recv() {
        size_t head = head_.v.load(std::memory_order_relaxed);
        size_t tail = tail_.v.load(std::memory_order_acquire);

        if (head == tail)
            return std::nullopt; // 비어 있음

        T item = std::move(buffer_[head & mask_]);
        head_.v.store(head + 1, std::memory_order_release);

        // 대기 중인 생산자를 깨움
        wake_waiter();

        return item;
    }

    // -------------------------------------------------------------------------
    // 비동기 send / recv (코루틴)
    // -------------------------------------------------------------------------

    /**
     * @brief 아이템을 전송합니다. 채널이 가득 차면 co_await 대기합니다.
     *
     * @param item 전송할 아이템.
     * @returns `Result<void>::ok()` 또는 `errc::broken_pipe` (닫힘).
     */
    Task<Result<void>> send(T item) {
        for (;;) {
            if (closed_.load(std::memory_order_relaxed))
                co_return unexpected(std::make_error_code(std::errc::broken_pipe));

            if (try_send_impl(std::move(item))) {
                co_return Result<void>{};
            }

            // 가득 참 — 소비자가 공간을 만들 때까지 대기
            co_await SendAwaiter{this};

            if (closed_.load(std::memory_order_relaxed))
                co_return unexpected(std::make_error_code(std::errc::broken_pipe));
        }
    }

    /**
     * @brief 아이템을 수신합니다. 채널이 비어 있으면 co_await 대기합니다.
     *
     * 채널이 닫히고 비면 `std::nullopt` (EOS) 반환.
     *
     * @returns 아이템 또는 `std::nullopt` (EOS).
     */
    Task<std::optional<T>> recv() {
        for (;;) {
            auto item = try_recv();
            if (item)
                co_return item;

            if (closed_.load(std::memory_order_acquire))
                co_return std::nullopt; // EOS

            // 비어 있음 — 생산자가 아이템을 넣을 때까지 대기
            co_await RecvAwaiter{this};

            // 재확인 (wake 후 다시 루프)
        }
    }

    // -------------------------------------------------------------------------
    // 수명 주기
    // -------------------------------------------------------------------------

    /**
     * @brief 채널을 닫습니다 (EOS 전파).
     *
     * `close()` 후 `send()`는 에러 반환.
     * `recv()`는 남은 아이템 소진 후 `std::nullopt` 반환.
     */
    void close() {
        closed_.store(true, std::memory_order_release);
        // 대기 중인 수신자/송신자 모두 깨움
        wake_waiter();
    }

    /**
     * @brief 채널이 닫혔는지 확인합니다.
     */
    [[nodiscard]] bool is_closed() const noexcept {
        return closed_.load(std::memory_order_relaxed);
    }

    /**
     * @brief 현재 채널의 근사 아이템 수를 반환합니다.
     */
    [[nodiscard]] size_t size_approx() const noexcept {
        size_t head = head_.v.load(std::memory_order_relaxed);
        size_t tail = tail_.v.load(std::memory_order_relaxed);
        return tail >= head ? tail - head : 0;
    }

    /**
     * @brief 채널의 용량을 반환합니다 (항상 2의 거듭제곱).
     */
    [[nodiscard]] size_t capacity() const noexcept {
        return mask_ + 1;
    }

private:
    // -------------------------------------------------------------------------
    // 캐시 라인 분리 head/tail
    // -------------------------------------------------------------------------

    struct alignas(64) Head { std::atomic<size_t> v{0}; };
    struct alignas(64) Tail { std::atomic<size_t> v{0}; };

    // -------------------------------------------------------------------------
    // 내부 try_send 구현
    // -------------------------------------------------------------------------

    bool try_send_impl(T&& item) {
        if (closed_.load(std::memory_order_relaxed))
            return false;

        size_t tail = tail_.v.load(std::memory_order_relaxed);
        size_t head = head_.v.load(std::memory_order_acquire);

        // 링 버퍼 가득 참 확인
        if (tail - head >= capacity())
            return false;

        buffer_[tail & mask_] = std::move(item);
        tail_.v.store(tail + 1, std::memory_order_release);

        // 대기 중인 수신자를 깨움
        wake_waiter();
        return true;
    }

    // -------------------------------------------------------------------------
    // Waiter (단일 대기자 — SPSC이므로 동시에 하나만 존재)
    // -------------------------------------------------------------------------

    void wake_waiter() {
        auto h = waiter_.exchange(nullptr, std::memory_order_acq_rel);
        if (!h) return;

        auto* reactor = waiter_reactor_.exchange(nullptr, std::memory_order_acq_rel);
        if (reactor)
            reactor->post([h]() mutable { h.resume(); });
        else
            h.resume();
    }

    // -------------------------------------------------------------------------
    // Send awaiter (생산자가 공간 생길 때까지 대기)
    // -------------------------------------------------------------------------
    struct SendAwaiter {
        SpscChannel<T>* chan;

        bool await_ready() const noexcept {
            if (chan->closed_.load(std::memory_order_relaxed))
                return true;
            size_t tail = chan->tail_.v.load(std::memory_order_relaxed);
            size_t head = chan->head_.v.load(std::memory_order_acquire);
            return (tail - head) < chan->capacity();
        }

        bool await_suspend(std::coroutine_handle<> h) {
            chan->waiter_.store(h, std::memory_order_release);
            chan->waiter_reactor_.store(Reactor::current(), std::memory_order_release);

            // Re-check after registering — avoids lost wake
            size_t tail = chan->tail_.v.load(std::memory_order_relaxed);
            size_t head = chan->head_.v.load(std::memory_order_acquire);
            if ((tail - head) < chan->capacity() || chan->closed_.load(std::memory_order_relaxed)) {
                // No longer need to wait — cancel registration
                chan->waiter_.store(nullptr, std::memory_order_release);
                chan->waiter_reactor_.store(nullptr, std::memory_order_release);
                return false;
            }
            return true;
        }

        void await_resume() noexcept {}
    };

    // -------------------------------------------------------------------------
    // Recv awaiter (소비자가 아이템 기다림)
    // -------------------------------------------------------------------------
    struct RecvAwaiter {
        SpscChannel<T>* chan;

        bool await_ready() const noexcept {
            if (chan->closed_.load(std::memory_order_relaxed))
                return true;
            size_t head = chan->head_.v.load(std::memory_order_relaxed);
            size_t tail = chan->tail_.v.load(std::memory_order_acquire);
            return head != tail;
        }

        bool await_suspend(std::coroutine_handle<> h) {
            chan->waiter_.store(h, std::memory_order_release);
            chan->waiter_reactor_.store(Reactor::current(), std::memory_order_release);

            // Re-check after registering
            size_t head = chan->head_.v.load(std::memory_order_relaxed);
            size_t tail = chan->tail_.v.load(std::memory_order_acquire);
            if (head != tail || chan->closed_.load(std::memory_order_relaxed)) {
                chan->waiter_.store(nullptr, std::memory_order_release);
                chan->waiter_reactor_.store(nullptr, std::memory_order_release);
                return false;
            }
            return true;
        }

        void await_resume() noexcept {}
    };

    // -------------------------------------------------------------------------
    // 2의 거듭제곱 올림 유틸리티
    // -------------------------------------------------------------------------
    static size_t next_pow2(size_t n) {
        if (n == 0) return 1;
        size_t p = 1;
        while (p < n) p <<= 1;
        return p;
    }

    // -------------------------------------------------------------------------
    // 데이터 멤버
    // -------------------------------------------------------------------------
    size_t        mask_;          ///< capacity - 1 (비트 마스크)
    std::vector<T> buffer_;       ///< 링 버퍼

    Head head_;  ///< 소비자 포인터 (캐시 라인 분리)
    Tail tail_;  ///< 생산자 포인터 (캐시 라인 분리)

    std::atomic<bool> closed_{false};

    // 단일 대기자 (SPSC — 동시에 하나만 존재)
    std::atomic<std::coroutine_handle<>> waiter_{nullptr};
    std::atomic<Reactor*>                waiter_reactor_{nullptr};
};

} // namespace qbuem

/** @} */
