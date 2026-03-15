#pragma once

/**
 * @file qbuem/pipeline/async_channel.hpp
 * @brief MPMC 비동기 채널 — Dmitry Vyukov ring buffer
 * @defgroup qbuem_async_channel AsyncChannel
 * @ingroup qbuem_pipeline
 *
 * AsyncChannel<T>는 다중 생산자/다중 소비자 비동기 채널입니다.
 *
 * ## 구현
 * Dmitry Vyukov의 MPMC ring buffer 알고리즘을 기반으로 합니다.
 * - `head_`/`tail_` cache-line 분리 (`alignas(64)`)
 * - `send()` / `recv()` — backpressure: 포화/비면 co_await 대기
 * - `try_send()` / `try_recv()` — wait-free, 논블로킹
 * - `close()` + EOS 전파
 * - Cross-reactor wakeup: `waiter.reactor->post([h]{h.resume();})`
 *
 * ## MPMC 메모리 순서 (Dmitry Vyukov 명세)
 * - send: `sequence.load(acquire)` → CAS(relaxed,relaxed) → data write
 *         → `sequence.store(tail+1, release)`
 * - recv: `sequence.load(acquire)` → data read
 *         → `sequence.store(head+capacity, release)`
 *
 * ## 사용 예시
 * @code
 * AsyncChannel<int> chan(1024);
 *
 * // 생산자 (다른 코루틴):
 * co_await chan.send(42);
 *
 * // 소비자:
 * auto item = co_await chan.recv(); // std::optional<int>
 * if (!item) { // EOS
 * }
 * @endcode
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/reactor.hpp>
#include <qbuem/core/task.hpp>

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace qbuem {

/**
 * @brief MPMC 비동기 채널.
 *
 * @tparam T 전송할 값의 타입 (이동 가능해야 함).
 */
template <typename T>
class AsyncChannel {
public:
  /**
   * @brief 지정한 용량으로 채널을 생성합니다.
   *
   * @param capacity 링 버퍼 용량. 2의 거듭제곱 권장 (내부에서 반올림).
   */
  explicit AsyncChannel(size_t capacity)
      : capacity_(next_pow2(std::max(capacity, size_t{2}))) {
    slots_ = new Slot[capacity_];
    for (size_t i = 0; i < capacity_; ++i)
      slots_[i].sequence.store(i, std::memory_order_relaxed);
  }

  ~AsyncChannel() { delete[] slots_; }

  AsyncChannel(const AsyncChannel &) = delete;
  AsyncChannel &operator=(const AsyncChannel &) = delete;

  // -------------------------------------------------------------------------
  // try_send / try_recv (lock-free, wait-free)
  // -------------------------------------------------------------------------

  /**
   * @brief 아이템을 채널에 넣으려 시도합니다 (논블로킹).
   *
   * @returns `true`이면 성공, `false`이면 포화(ring full) 또는 닫힘.
   */
  bool try_send(T value) {
    if (closed_.load(std::memory_order_relaxed))
      return false;

    size_t pos = tail_.load(std::memory_order_relaxed);
    for (;;) {
      Slot &slot = slots_[pos & (capacity_ - 1)];
      size_t seq = slot.sequence.load(std::memory_order_acquire);
      intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

      if (diff == 0) {
        // Slot is free: try to claim it
        if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed,
                                        std::memory_order_relaxed)) {
          slot.storage = std::move(value);
          slot.sequence.store(pos + 1, std::memory_order_release);
          wake_one_receiver();
          return true;
        }
      } else if (diff < 0) {
        return false; // ring full
      } else {
        pos = tail_.load(std::memory_order_relaxed);
      }
    }
  }

  /**
   * @brief 채널에서 아이템을 꺼내려 시도합니다 (논블로킹).
   *
   * @returns 아이템이 있으면 `std::optional<T>`, 없으면 `std::nullopt`.
   *          닫힌 채널에서 빈 경우 `std::nullopt` (EOS).
   */
  std::optional<T> try_recv() {
    size_t pos = head_.load(std::memory_order_relaxed);
    for (;;) {
      Slot &slot = slots_[pos & (capacity_ - 1)];
      size_t seq = slot.sequence.load(std::memory_order_acquire);
      intptr_t diff =
          static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

      if (diff == 0) {
        // Slot has data: try to claim it
        if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
          T value = std::move(slot.storage);
          slot.sequence.store(pos + capacity_, std::memory_order_release);
          wake_one_sender();
          return value;
        }
      } else if (diff < 0) {
        return std::nullopt; // ring empty
      } else {
        pos = head_.load(std::memory_order_relaxed);
      }
    }
  }

  // -------------------------------------------------------------------------
  // Async send / recv (backpressure via co_await)
  // -------------------------------------------------------------------------

  /**
   * @brief 아이템을 전송합니다. 채널이 가득 차면 co_await 대기합니다.
   *
   * 채널이 닫혀 있으면 즉시 에러를 반환합니다.
   *
   * @returns `Result<void>::ok()` 또는 `errc::broken_pipe` (채널 닫힘).
   */
  Task<Result<void>> send(T value) {
    for (;;) {
      if (closed_.load(std::memory_order_relaxed))
        co_return unexpected(std::make_error_code(std::errc::broken_pipe));

      if (try_send_inner(value)) {
        wake_one_receiver();
        co_return Result<void>{};
      }

      // 채널 가득 참 — 소비자 공간 생길 때까지 대기
      co_await SendAwaiter{this};
    }
  }

  /**
   * @brief 아이템을 수신합니다. 채널이 비면 co_await 대기합니다.
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

      // 채널 비어 있음 — 아이템 도착 대기
      co_await RecvAwaiter{this};
    }
  }

  // -------------------------------------------------------------------------
  // Batch operations
  // -------------------------------------------------------------------------

  /**
   * @brief 최대 `max_n`개 아이템을 배치로 꺼냅니다 (논블로킹).
   *
   * @param out   결과를 채울 버퍼.
   * @param max_n 최대 꺼낼 개수 (0이면 out.size()).
   * @returns 실제로 꺼낸 개수.
   */
  size_t try_recv_batch(std::span<T> out, size_t max_n = 0) {
    if (max_n == 0 || max_n > out.size())
      max_n = out.size();
    size_t n = 0;
    while (n < max_n) {
      auto item = try_recv();
      if (!item)
        break;
      out[n++] = std::move(*item);
    }
    return n;
  }

  /**
   * @brief 여러 아이템을 배치로 전송합니다 (논블로킹).
   *
   * @param items 전송할 아이템들.
   * @returns 실제로 전송한 개수.
   */
  size_t send_batch(std::span<T> items) {
    size_t n = 0;
    for (auto &item : items) {
      if (!try_send(std::move(item)))
        break;
      ++n;
    }
    return n;
  }

  // -------------------------------------------------------------------------
  // Lifecycle
  // -------------------------------------------------------------------------

  /**
   * @brief 채널을 닫습니다 (EOS 전파).
   *
   * `close()` 후 `send()`는 에러 반환.
   * `recv()`는 남은 아이템 소진 후 `std::nullopt` 반환.
   */
  void close() {
    closed_.store(true, std::memory_order_release);
    wake_all_receivers();
    wake_all_senders();
  }

  /**
   * @brief 채널이 닫혔는지 확인합니다.
   */
  [[nodiscard]] bool is_closed() const noexcept {
    return closed_.load(std::memory_order_relaxed);
  }

  /**
   * @brief 현재 채널 용량을 반환합니다.
   */
  [[nodiscard]] size_t capacity() const noexcept { return capacity_; }

  /**
   * @brief 현재 채널에 있는 아이템 수를 근사적으로 반환합니다.
   *
   * @note 멀티스레드 환경에서는 반환 즉시 변할 수 있습니다.
   */
  [[nodiscard]] size_t size_approx() const noexcept {
    size_t head = head_.load(std::memory_order_relaxed);
    size_t tail = tail_.load(std::memory_order_relaxed);
    return tail >= head ? tail - head : 0;
  }

private:
  // -------------------------------------------------------------------------
  // Ring buffer slot
  // -------------------------------------------------------------------------
  struct alignas(64) Slot {
    std::atomic<size_t> sequence{0};
    T storage;
  };

  // -------------------------------------------------------------------------
  // Awaiter Waiter (send/recv)
  // -------------------------------------------------------------------------
  struct Waiter {
    std::coroutine_handle<> handle;
    Reactor                *reactor;
    Waiter                 *next = nullptr;
  };

  // -------------------------------------------------------------------------
  // Send awaiter (waits for space)
  // -------------------------------------------------------------------------
  struct SendAwaiter {
    AsyncChannel<T> *chan;
    Waiter           waiter;

    bool await_ready() const noexcept {
      return !chan->is_full() || chan->is_closed();
    }

    bool await_suspend(std::coroutine_handle<> h) {
      waiter.handle  = h;
      waiter.reactor = Reactor::current();
      chan->add_send_waiter(&waiter);
      // Re-check: space may have appeared after adding waiter
      if (!chan->is_full() || chan->is_closed()) {
        // If remove returns false, someone already removed us and will resume us
        if (chan->remove_send_waiter(&waiter))
          return false; // we removed ourselves — continue inline
        // else: already removed by wake_one_sender — stay suspended, it will resume us
      }
      return true;
    }

    void await_resume() noexcept {}
  };

  // -------------------------------------------------------------------------
  // Recv awaiter (waits for item)
  // -------------------------------------------------------------------------
  struct RecvAwaiter {
    AsyncChannel<T> *chan;
    Waiter           waiter;

    bool await_ready() const noexcept {
      return !chan->is_empty() || chan->is_closed();
    }

    bool await_suspend(std::coroutine_handle<> h) {
      waiter.handle  = h;
      waiter.reactor = Reactor::current();
      chan->add_recv_waiter(&waiter);
      if (!chan->is_empty() || chan->is_closed()) {
        // If remove returns false, someone already removed us and will resume us
        if (chan->remove_recv_waiter(&waiter))
          return false; // we removed ourselves — continue inline
        // else: already removed by wake_one_receiver — stay suspended, it will resume us
      }
      return true;
    }

    void await_resume() noexcept {}
  };

  // -------------------------------------------------------------------------
  // Internal helpers
  // -------------------------------------------------------------------------

  bool try_send_inner(T &value) {
    size_t pos = tail_.load(std::memory_order_relaxed);
    for (;;) {
      Slot &slot = slots_[pos & (capacity_ - 1)];
      size_t seq = slot.sequence.load(std::memory_order_acquire);
      intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

      if (diff == 0) {
        if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed,
                                        std::memory_order_relaxed)) {
          slot.storage = std::move(value);
          slot.sequence.store(pos + 1, std::memory_order_release);
          return true;
        }
      } else if (diff < 0) {
        return false;
      } else {
        pos = tail_.load(std::memory_order_relaxed);
      }
    }
  }

  bool is_full() const noexcept {
    size_t tail = tail_.load(std::memory_order_relaxed);
    size_t head = head_.load(std::memory_order_relaxed);
    return (tail - head) >= capacity_;
  }

  bool is_empty() const noexcept {
    size_t tail = tail_.load(std::memory_order_relaxed);
    size_t head = head_.load(std::memory_order_relaxed);
    return tail == head;
  }

  void add_send_waiter(Waiter *w) {
    std::lock_guard lock(send_waiters_mutex_);
    w->next         = send_waiters_;
    send_waiters_   = w;
  }

  bool remove_send_waiter(Waiter *w) {
    std::lock_guard lock(send_waiters_mutex_);
    Waiter **p = &send_waiters_;
    while (*p) {
      if (*p == w) { *p = w->next; return true; }
      p = &(*p)->next;
    }
    return false;
  }

  void add_recv_waiter(Waiter *w) {
    std::lock_guard lock(recv_waiters_mutex_);
    w->next         = recv_waiters_;
    recv_waiters_   = w;
  }

  bool remove_recv_waiter(Waiter *w) {
    std::lock_guard lock(recv_waiters_mutex_);
    Waiter **p = &recv_waiters_;
    while (*p) {
      if (*p == w) { *p = w->next; return true; }
      p = &(*p)->next;
    }
    return false;
  }

  void wake_one_receiver() {
    Waiter *w = nullptr;
    {
      std::lock_guard lock(recv_waiters_mutex_);
      if (recv_waiters_) {
        w              = recv_waiters_;
        recv_waiters_  = w->next;
      }
    }
    if (!w) return;
    resume_waiter(w);
  }

  void wake_one_sender() {
    Waiter *w = nullptr;
    {
      std::lock_guard lock(send_waiters_mutex_);
      if (send_waiters_) {
        w             = send_waiters_;
        send_waiters_ = w->next;
      }
    }
    if (!w) return;
    resume_waiter(w);
  }

  void wake_all_receivers() {
    Waiter *list = nullptr;
    {
      std::lock_guard lock(recv_waiters_mutex_);
      list           = recv_waiters_;
      recv_waiters_  = nullptr;
    }
    while (list) {
      Waiter *next = list->next;
      resume_waiter(list);
      list = next;
    }
  }

  void wake_all_senders() {
    Waiter *list = nullptr;
    {
      std::lock_guard lock(send_waiters_mutex_);
      list          = send_waiters_;
      send_waiters_ = nullptr;
    }
    while (list) {
      Waiter *next = list->next;
      resume_waiter(list);
      list = next;
    }
  }

  static void resume_waiter(Waiter *w) {
    // 반드시 owning Reactor 스레드에서 resume — cross-reactor 안전
    if (w->reactor)
      w->reactor->post([h = w->handle]() mutable { h.resume(); });
    else
      w->handle.resume();
  }

  static size_t next_pow2(size_t n) {
    if (n == 0) return 1;
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
  }

  // -------------------------------------------------------------------------
  // Data members
  // -------------------------------------------------------------------------
  const size_t capacity_;
  Slot        *slots_;

  alignas(64) std::atomic<size_t> tail_{0};  // producers
  alignas(64) std::atomic<size_t> head_{0};  // consumers
  alignas(64) std::atomic<bool>   closed_{false};

  // Waiter lists (protected by their respective mutexes)
  std::mutex send_waiters_mutex_;
  Waiter    *send_waiters_ = nullptr;

  std::mutex recv_waiters_mutex_;
  Waiter    *recv_waiters_ = nullptr;
};

} // namespace qbuem

/** @} */
