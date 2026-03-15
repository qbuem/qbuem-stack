#pragma once

/**
 * @file qbuem/db/connection_pool.hpp
 * @brief LockFreeConnectionPool — O(1) lock-free 연결 풀 구현.
 * @defgroup qbuem_db_pool LockFreeConnectionPool
 * @ingroup qbuem_db
 *
 * ## 개요
 * `IConnectionPool`의 구체 구현입니다.
 * Dmitry Vyukov의 MPMC 링 버퍼를 응용한 **lock-free** 연결 슬롯 인덱싱으로
 * O(1) `acquire()` / `release()`를 보장합니다.
 *
 * ## 설계
 * ```
 * [free_ring_] ─── 사용 가능한 슬롯 인덱스 링 버퍼
 *     │
 *     ▼
 * [slots_[N]] ─── 각 슬롯: IConnection* + 상태 원자값
 * ```
 *
 * ## acquire() 알고리즘
 * 1. `free_ring_.try_pop()` → 슬롯 인덱스 획득 (O(1) lock-free).
 * 2. 연결이 살아 있으면 반환, 아니면 `reconnect()`.
 * 3. 연결 없으면 AsyncChannel waiter에 등록 → 반환 시 wake.
 *
 * ## 연결 수명 관리
 * - `min_size` 연결은 항상 유지 (idle timeout 면제).
 * - `max_size` 이상은 생성하지 않음 — backpressure.
 * - idle 연결은 `idle_timeout_ms` 후 자동 해제.
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/core/timer_wheel.hpp>
#include <qbuem/db/driver.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace qbuem::db {

// ─── 슬롯 상태 ───────────────────────────────────────────────────────────────

/**
 * @brief 연결 슬롯 내부 상태.
 */
enum class SlotState : uint8_t {
    Free      = 0,  ///< 풀에서 사용 가능
    Active    = 1,  ///< 호출자가 사용 중
    Recycling = 2,  ///< idle timeout 처리 중
    Dead      = 3,  ///< 연결 실패 / 종료됨
};

// ─── LockFreeConnectionPool ──────────────────────────────────────────────────

/**
 * @brief Lock-free O(1) DB 연결 풀.
 *
 * `IConnectionPool` 인터페이스 구현체입니다.
 * 구체 드라이버는 생성자에 `factory` 콜백으로 주입합니다.
 *
 * @note 이 클래스는 `IDBDriver::pool()`에서 반환되는 것이 아니라
 *       드라이버 없이 직접 연결 팩토리를 주입할 수 있는 범용 풀입니다.
 */
class LockFreeConnectionPool final : public IConnectionPool {
public:
    /** @brief 연결 생성 팩토리 함수 타입. */
    using ConnFactory = std::function<Task<Result<std::unique_ptr<IConnection>>>()>;

    /**
     * @brief LockFreeConnectionPool을 생성합니다.
     *
     * @param factory  새 연결을 생성하는 비동기 팩토리.
     * @param config   풀 설정.
     */
    explicit LockFreeConnectionPool(ConnFactory factory,
                                     PoolConfig  config = {}) noexcept;
    ~LockFreeConnectionPool() override;

    // ── IConnectionPool 구현 ─────────────────────────────────────────────

    /**
     * @brief O(1) lock-free 연결 획득.
     *
     * free_ring에서 슬롯 인덱스를 pop한 후, 연결 상태를 확인합니다.
     * 연결이 유효하지 않으면 재연결 후 반환합니다.
     * 슬롯이 없으면 max_size 미만이면 새 연결 생성, 아니면 waiter 큐 대기.
     */
    Task<Result<std::unique_ptr<IConnection>>> acquire() override;

    [[nodiscard]] size_t active_count() const noexcept override {
        return active_count_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] size_t idle_count() const noexcept override {
        return idle_count_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] size_t max_size() const noexcept override {
        return config_.max_size;
    }

    Task<void> drain() override;

    // ── 초기화 ───────────────────────────────────────────────────────────

    /**
     * @brief 최소 연결(`min_size`)을 미리 생성합니다.
     *
     * 서버 시작 시 풀을 워밍업하여 첫 요청의 레이턴시를 줄입니다.
     */
    Task<Result<void>> warmup() noexcept;

private:
    // ─── 내부 슬롯 구조 ──────────────────────────────────────────────────
    struct alignas(64) Slot {
        std::unique_ptr<IConnection> conn;
        std::atomic<SlotState>       state{SlotState::Free};
        uint64_t                     last_used_ms{0}; ///< 마지막 사용 시각
        uint32_t                     index{0};        ///< 슬롯 인덱스 (자기 참조)
        uint8_t                      _pad[64 - sizeof(void*)
                                              - sizeof(std::atomic<SlotState>)
                                              - sizeof(uint64_t)
                                              - sizeof(uint32_t)]{};
    };

    // ─── lock-free 스택 기반 free slot 관리 ─────────────────────────────
    // Vyukov MPMC 대신 단순 LIFO 스택 (연결 풀은 순서 무관)
    struct FreeStack {
        static constexpr size_t kMaxSlots = 256;

        alignas(64) std::atomic<size_t> top_{0};
        alignas(64) std::atomic<uint32_t> slots_[kMaxSlots]{};

        explicit FreeStack() = default;

        bool push(uint32_t idx) noexcept {
            size_t t = top_.load(std::memory_order_relaxed);
            if (t >= kMaxSlots) return false;
            // CAS 기반 push
            while (!top_.compare_exchange_weak(t, t + 1,
                       std::memory_order_release, std::memory_order_relaxed)) {
                if (t >= kMaxSlots) return false;
            }
            slots_[t].store(idx, std::memory_order_release);
            return true;
        }

        std::optional<uint32_t> pop() noexcept {
            size_t t = top_.load(std::memory_order_acquire);
            while (t > 0) {
                if (top_.compare_exchange_weak(t, t - 1,
                        std::memory_order_release, std::memory_order_relaxed)) {
                    return slots_[t - 1].load(std::memory_order_acquire);
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] size_t size() const noexcept {
            return top_.load(std::memory_order_relaxed);
        }
    };

    // ─── waiter 리스트 ───────────────────────────────────────────────────
    struct Waiter {
        std::coroutine_handle<> handle;
        Reactor*                reactor;
        uint32_t                slot_idx; ///< 할당된 슬롯 인덱스 (wake 후 설정)
        Waiter*                 next{nullptr};
    };

    void enqueue_waiter(Waiter* w) noexcept;
    void wake_one_waiter(uint32_t slot_idx) noexcept;

    // ─── 연결 재활용 ────────────────────────────────────────────────────
    Task<bool> validate_or_reconnect(Slot& slot) noexcept;
    void       release_slot(uint32_t slot_idx) noexcept;

    // ─── idle timeout 정리 ──────────────────────────────────────────────
    void schedule_idle_cleanup() noexcept;

    // ─── 데이터 멤버 ────────────────────────────────────────────────────
    ConnFactory  factory_;
    PoolConfig   config_;

    // 슬롯 배열 (고정 크기, max_size 기준)
    std::vector<Slot>     slots_;
    FreeStack             free_stack_;

    std::atomic<size_t>   total_count_{0};
    std::atomic<size_t>   active_count_{0};
    std::atomic<size_t>   idle_count_{0};

    // waiter 리스트
    std::mutex            waiter_mutex_;
    Waiter*               waiter_head_{nullptr};
    std::atomic<uint32_t> waiter_count_{0};

    std::atomic<bool>     draining_{false};
};

// ─── RAII 연결 가드 ──────────────────────────────────────────────────────────

/**
 * @brief 풀에서 획득한 연결을 RAII로 관리하는 가드.
 *
 * 소멸 시 자동으로 풀에 반환됩니다.
 *
 * @code
 * auto guard = co_await PooledConnection::acquire(pool);
 * auto& conn = guard.get();
 * auto stmt = co_await conn.prepare("SELECT 1");
 * // guard 소멸 → 자동 반환
 * @endcode
 */
class PooledConnection {
public:
    PooledConnection() = default;

    PooledConnection(std::unique_ptr<IConnection> conn,
                     IConnectionPool*             pool) noexcept
        : conn_(std::move(conn)), pool_(pool) {}

    ~PooledConnection() {
        // IConnection 소멸자에서 풀 반환 로직을 트리거
        // (구체 Connection이 pool 포인터를 알고 있어야 함)
        conn_.reset();
    }

    PooledConnection(PooledConnection&&) noexcept = default;
    PooledConnection& operator=(PooledConnection&&) noexcept = default;

    [[nodiscard]] IConnection& get() noexcept { return *conn_; }
    [[nodiscard]] const IConnection& get() const noexcept { return *conn_; }

    [[nodiscard]] IConnection* operator->() noexcept { return conn_.get(); }
    [[nodiscard]] bool valid() const noexcept { return conn_ != nullptr; }

    /**
     * @brief 풀에서 연결을 획득하는 팩토리 함수.
     */
    static Task<Result<PooledConnection>> acquire(IConnectionPool& pool) {
        auto r = co_await pool.acquire();
        if (!r) co_return unexpected(r.error());
        co_return PooledConnection(std::move(*r), &pool);
    }

private:
    std::unique_ptr<IConnection> conn_;
    IConnectionPool*             pool_{nullptr};
};

} // namespace qbuem::db

/** @} */
