#pragma once

/**
 * @file qbuem/pipeline/arena_channel.hpp
 * @brief Reactor-local zero-alloc 채널 — ArenaChannel<T>
 * @defgroup qbuem_arena_channel ArenaChannel
 * @ingroup qbuem_pipeline
 *
 * ArenaChannel<T>는 동일 reactor 내부의 producer/consumer 간 zero-copy,
 * zero-heap-alloc 메시지 전달을 위한 채널입니다.
 *
 * ## 설계 원칙
 *
 * AsyncChannel<T>은 new/delete 기반 힙 슬롯을 사용합니다. reactor-local
 * 통신(같은 스레드 내 coroutine 간)에서는 이 할당이 불필요합니다.
 * ArenaChannel은 FixedPoolResource를 통해 슬롯 할당/해제를 O(1) free-list
 * 연산으로 교체하고 malloc 캐시 미스를 제거합니다.
 *
 * ### 사용 조건
 * - **같은 reactor(스레드) 내** producer와 consumer 사이에서만 안전합니다.
 * - 크로스-reactor 접근 시 외부에서 동기화를 제공해야 합니다.
 *   (크로스-reactor 용도라면 AsyncChannel<T>을 사용하세요.)
 *
 * ### 메모리 모델
 * ```
 * FixedPoolResource<sizeof(Node)>  ←──────────────────────┐
 *        │  allocate() O(1)                                │ deallocate()
 *        ▼                                                 │
 *   [Node: value + next ptr]──linked list──► head          │
 *        ▲                                                 │
 *   push: placement new into pool slot      pop: call ~T, return slot
 * ```
 *
 * ## 사용 예시
 * ```cpp
 * ArenaChannel<int> chan(256); // 최대 256 슬롯 (heap 1회만 할당)
 *
 * // 같은 reactor 내 producer:
 * chan.push(42);          // true → success, false → full
 *
 * // 같은 reactor 내 consumer:
 * auto v = chan.pop();    // std::optional<int>
 * if (v) process(*v);
 * ```
 *
 * ## AsyncChannel 과의 비교
 * | 특성              | AsyncChannel<T>    | ArenaChannel<T>         |
 * |-------------------|--------------------|-------------------------|
 * | 할당 방식         | new/delete (힙)    | FixedPoolResource (풀)  |
 * | 스레드 안전성     | MPMC (atomic CAS)  | 단일 reactor (no lock)  |
 * | co_await 지원     | ✅                 | ❌ (sync only)          |
 * | 용도              | 크로스-reactor     | 동일 reactor 내         |
 * | 할당 비용         | O(1) amortized     | O(1) 항상               |
 *
 * @{
 */

#include <qbuem/core/arena.hpp>

#include <cstddef>
#include <new>
#include <optional>
#include <type_traits>

namespace qbuem {

/**
 * @brief Reactor-local zero-alloc SPSC/SPMC 채널.
 *
 * 내부적으로 FixedPoolResource 위에 intrusive linked list 큐를 구성합니다.
 * 모든 연산은 lock-free이며 단일 reactor 내에서만 안전합니다.
 *
 * @tparam T 전송할 값의 타입. 이동 가능(MoveConstructible)해야 합니다.
 * @tparam Alignment 슬롯 정렬 (기본 64 — cache-line boundary).
 */
template <typename T, size_t Alignment = 64>
class ArenaChannel {
  // -------------------------------------------------------------------------
  // Internal linked-list node
  // -------------------------------------------------------------------------
  struct alignas(Alignment) Node {
    T       value;
    Node   *next{nullptr};

    template <typename... Args>
    explicit Node(Args&&... args)
        : value(std::forward<Args>(args)...) {}
  };

  static constexpr size_t kNodeSize = sizeof(Node);

public:
  /**
   * @brief 지정한 슬롯 수로 채널을 생성합니다.
   *
   * 이 시점에 `capacity * sizeof(Node)` 바이트를 **1회** 힙 할당합니다.
   * 이후 push/pop은 추가 힙 할당 없이 free-list에서 O(1) 처리됩니다.
   *
   * @param capacity 동시에 보관 가능한 최대 메시지 수.
   */
  explicit ArenaChannel(size_t capacity)
      : pool_(capacity) {}

  ArenaChannel(const ArenaChannel &) = delete;
  ArenaChannel &operator=(const ArenaChannel &) = delete;
  ArenaChannel(ArenaChannel &&) = delete;
  ArenaChannel &operator=(ArenaChannel &&) = delete;

  ~ArenaChannel() {
    // Drain remaining items to run T destructors
    while (head_) {
      Node *n = head_;
      head_ = n->next;
      n->~Node();
      pool_.deallocate(n);
    }
  }

  // -------------------------------------------------------------------------
  // Producer API
  // -------------------------------------------------------------------------

  /**
   * @brief 값을 채널에 넣습니다 (O(1), zero-heap-alloc).
   *
   * @param value 전송할 값 (이동됩니다).
   * @returns true  — 성공.
   * @returns false — 풀이 가득 찬 경우 (backpressure).
   */
  bool push(T value) {
    void *slot = pool_.allocate();
    if (!slot) [[unlikely]] return false;

    Node *n = ::new (slot) Node(std::move(value));
    if (tail_) {
      tail_->next = n;
    } else {
      head_ = n;
    }
    tail_ = n;
    ++size_;
    return true;
  }

  /**
   * @brief 값을 in-place 생성하여 채널에 넣습니다 (O(1)).
   *
   * @tparam Args T 생성자 인수 타입.
   * @param  args T 생성자에 전달할 인수.
   * @returns true  — 성공.
   * @returns false — 풀이 가득 찬 경우.
   */
  template <typename... Args>
  bool emplace(Args&&... args) {
    void *slot = pool_.allocate();
    if (!slot) [[unlikely]] return false;

    Node *n = ::new (slot) Node(std::forward<Args>(args)...);
    if (tail_) {
      tail_->next = n;
    } else {
      head_ = n;
    }
    tail_ = n;
    ++size_;
    return true;
  }

  // -------------------------------------------------------------------------
  // Consumer API
  // -------------------------------------------------------------------------

  /**
   * @brief 채널에서 값 하나를 꺼냅니다 (O(1)).
   *
   * @returns 값이 있으면 `std::optional<T>`, 비어 있으면 `std::nullopt`.
   */
  std::optional<T> pop() {
    if (!head_) [[unlikely]] return std::nullopt;

    Node *n = head_;
    head_ = n->next;
    if (!head_) tail_ = nullptr;
    --size_;

    std::optional<T> result{std::move(n->value)};
    n->~Node();
    pool_.deallocate(n);
    return result;
  }

  /**
   * @brief 채널에서 최대 max_n개 값을 꺼내 out에 저장합니다 (배치 dequeue).
   *
   * @param out   결과를 저장할 버퍼 (push_back 지원).
   * @param max_n 최대 꺼낼 수 (0 = 제한 없음).
   * @returns 실제 꺼낸 수.
   */
  template <typename Container>
  size_t pop_batch(Container &out, size_t max_n = 0) {
    size_t count = 0;
    while (head_ && (max_n == 0 || count < max_n)) {
      Node *n = head_;
      head_ = n->next;
      if (!head_) tail_ = nullptr;
      --size_;

      out.push_back(std::move(n->value));
      n->~Node();
      pool_.deallocate(n);
      ++count;
    }
    return count;
  }

  // -------------------------------------------------------------------------
  // Introspection
  // -------------------------------------------------------------------------

  /// @brief 채널이 비어 있으면 true.
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

  /// @brief 현재 대기 중인 아이템 수.
  [[nodiscard]] size_t size() const noexcept { return size_; }

  /// @brief 풀의 남은 슬롯 수.
  [[nodiscard]] size_t available() const noexcept { return pool_.available(); }

  /// @brief 풀의 총 용량.
  [[nodiscard]] size_t capacity() const noexcept { return pool_.capacity(); }

private:
  FixedPoolResource<kNodeSize, Alignment> pool_;
  Node   *head_{nullptr};
  Node   *tail_{nullptr};
  size_t  size_{0};
};

/** @} */

} // namespace qbuem
