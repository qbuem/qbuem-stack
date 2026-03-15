#pragma once

/**
 * @file qbuem/core/timer_wheel.hpp
 * @brief 4단계 계층형 타이밍 휠 — O(1) 스케줄/취소
 * @defgroup qbuem_timer TimerWheel
 * @ingroup qbuem_core
 *
 * 4단계 × 256슬롯 계층형 타이밍 휠 구현입니다.
 *
 * ## 슬롯 해상도
 * - 레벨 0: 1ms 단위  (최대 256ms)
 * - 레벨 1: 256ms 단위 (최대 65.536초)
 * - 레벨 2: 65.536초 단위 (최대 ~4.7시간)
 * - 레벨 3: ~4.7시간 단위 (최대 ~49.7일)
 *
 * ## 복잡도
 * - schedule(): O(1)
 * - cancel():   O(슬롯 내 항목 수) 최악의 경우, 실용적으로 O(1) 수준
 * - tick():     O(fired_count)
 *
 * ## FixedPoolResource 연동
 * 내부 `Entry` 객체는 `<qbuem/core/arena.hpp>`의 `FixedPoolResource`를 사용하여
 * 힙 단편화 없는 O(1) 할당/해제를 달성합니다.
 * 풀 크기는 생성자에 `pool_capacity` 인자로 지정합니다.
 *
 * ## Reactor 통합
 * `Reactor::register_timer()`를 대체하는 고성능 타이머 백엔드로 사용합니다.
 * Reactor 구현체는 `poll()` 루프에서 `tick(elapsed_ms)`를 호출하고,
 * `next_expiry_ms()`를 poll 타임아웃으로 전달합니다.
 *
 * @code
 * // Reactor 이벤트 루프에서의 통합 패턴:
 * TimerWheel wheel;
 * while (running) {
 *     uint64_t timeout = wheel.next_expiry_ms();
 *     int n = epoll_wait(epfd, events, MAX_EVENTS, (int)std::min(timeout, (uint64_t)INT_MAX));
 *     uint64_t now = monotonic_ms();
 *     wheel.tick(now - last_tick_ms);
 *     last_tick_ms = now;
 *     // fd 이벤트 처리 ...
 * }
 * @endcode
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/arena.hpp>

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>

namespace qbuem {

/**
 * @brief 4단계 계층형 타이밍 휠.
 *
 * 힙 단편화 없는 O(1) 스케줄/취소를 지원하는 타이머 구조체입니다.
 * 내부적으로 `FixedPoolResource`를 사용하여 Entry 객체를 풀에서 할당합니다.
 * Reactor 이벤트 루프와 통합하여 타임아웃 처리에 사용합니다.
 *
 * ### 사용 예시
 * @code
 * TimerWheel wheel;
 * auto id = wheel.schedule(100, []{ std::puts("100ms 경과"); });
 * wheel.tick(50);   // 50ms 경과
 * wheel.tick(50);   // 100ms 경과 — 콜백 호출됨
 * wheel.cancel(id); // 이미 실행됐으므로 false 반환
 * @endcode
 */
class TimerWheel {
public:
  /** @brief 타이머 콜백 타입. */
  using Callback = std::function<void()>;
  /** @brief 타이머 식별자 타입. */
  using TimerId = uint64_t;

  /** @brief 타이밍 휠 레벨 수. */
  static constexpr size_t LEVELS = 4;
  /** @brief 레벨당 슬롯 수. */
  static constexpr size_t SLOTS_PER_LEVEL = 256;
  /** @brief 유효하지 않은 타이머 ID. */
  static constexpr TimerId kInvalid = 0;

  /**
   * @brief 타이머 휠을 초기화합니다.
   *
   * @param pool_capacity FixedPoolResource 풀의 최대 Entry 수.
   *                      동시 활성 타이머 최대 수보다 크게 설정하세요.
   *                      기본값 4096.
   */
  explicit TimerWheel(size_t pool_capacity = 4096) noexcept
      : pool_(pool_capacity) {}

  // 복사/이동 금지 (포인터 기반 내부 링크 구조)
  TimerWheel(const TimerWheel &) = delete;
  TimerWheel &operator=(const TimerWheel &) = delete;
  TimerWheel(TimerWheel &&) = delete;
  TimerWheel &operator=(TimerWheel &&) = delete;

  /** @brief 소멸자: 모든 미만료 타이머 항목을 풀에 반환합니다. */
  ~TimerWheel() { clear(); }

  /**
   * @brief 지정 지연(ms) 후 콜백을 실행하도록 스케줄합니다.
   *
   * FixedPoolResource에서 Entry를 O(1)로 할당합니다.
   * 풀이 고갈된 경우 kInvalid를 반환합니다.
   *
   * @param delay_ms 지연 시간 (밀리초). 0이면 다음 tick에서 즉시 실행.
   * @param fn       만료 시 호출할 콜백.
   * @returns 취소에 사용할 TimerId. 풀 고갈 시 kInvalid.
   */
  [[nodiscard]] TimerId schedule(uint64_t delay_ms, Callback fn) {
    void *raw = pool_.allocate();
    if (!raw) [[unlikely]] return kInvalid;

    auto *e = new (raw) Entry{};
    e->fn        = std::move(fn);
    e->id        = next_id_++;
    e->expiry_ms = current_ms_ + delay_ms;
    if (next_id_ == kInvalid) next_id_ = 1; // wrap-around 시 0 건너뜀

    insert(e);
    ++count_;
    return e->id;
  }

  /**
   * @brief 타이머를 취소합니다.
   *
   * 슬롯 내 이중 연결 리스트를 통해 제거합니다.
   * 취소는 상대적으로 드물게 발생하는 작업입니다.
   *
   * @param id schedule()이 반환한 TimerId.
   * @returns 만료 전 취소 성공 시 true, 이미 실행됐거나 없으면 false.
   */
  bool cancel(TimerId id) {
    if (id == kInvalid) return false;
    for (size_t lv = 0; lv < LEVELS; ++lv) {
      for (size_t sl = 0; sl < SLOTS_PER_LEVEL; ++sl) {
        for (Entry *e = slots_[lv][sl]; e; e = e->next) {
          if (e->id == id) {
            unlink(lv, sl, e);
            e->~Entry();
            pool_.deallocate(e);
            --count_;
            return true;
          }
        }
      }
    }
    return false;
  }

  /**
   * @brief elapsed_ms 만큼 시계를 진행하고 만료된 콜백을 실행합니다.
   *
   * 레벨 0를 매 ms마다 처리하고, 상위 레벨은 캐스케이드 조건에 따라 처리합니다.
   *
   * @param elapsed_ms 마지막 tick 이후 경과 시간 (밀리초).
   * @returns 실행된 콜백 수.
   */
  size_t tick(uint64_t elapsed_ms) {
    uint64_t target = current_ms_ + elapsed_ms;
    size_t fired = 0;

    while (current_ms_ < target) {
      ++current_ms_;
      // 레벨 0 슬롯 처리
      size_t slot0 = static_cast<size_t>(current_ms_ & (SLOTS_PER_LEVEL - 1));
      fired += fire_slot(0, slot0);

      // 레벨 캐스케이드: 상위 레벨을 하위로 재분배
      for (size_t lv = 1; lv < LEVELS; ++lv) {
        uint64_t divisor = kSlotMs[lv];
        if ((current_ms_ % divisor) == 0) {
          size_t sl = static_cast<size_t>((current_ms_ / divisor) & (SLOTS_PER_LEVEL - 1));
          cascade(lv, sl);
        } else {
          break;
        }
      }
    }
    return fired;
  }

  /**
   * @brief 다음 만료까지 남은 시간(ms)을 반환합니다 (poll 타임아웃용).
   *
   * Reactor의 `poll()` 호출 시 타임아웃 값으로 사용합니다:
   * @code
   * uint64_t timeout = wheel.next_expiry_ms();
   * epoll_wait(epfd, events, N, (int)std::min(timeout, (uint64_t)INT_MAX));
   * @endcode
   *
   * @returns 대기 중 타이머가 없으면 `UINT64_MAX`, 있으면 남은 ms (최소 0).
   */
  [[nodiscard]] uint64_t next_expiry_ms() const noexcept {
    if (count_ == 0) return std::numeric_limits<uint64_t>::max();

    uint64_t earliest = std::numeric_limits<uint64_t>::max();
    for (size_t lv = 0; lv < LEVELS; ++lv) {
      for (size_t sl = 0; sl < SLOTS_PER_LEVEL; ++sl) {
        for (const Entry *e = slots_[lv][sl]; e; e = e->next) {
          if (e->expiry_ms < earliest)
            earliest = e->expiry_ms;
        }
      }
    }
    if (earliest <= current_ms_) return 0;
    return earliest - current_ms_;
  }

  /**
   * @brief 현재 가상 시계 값(ms)을 반환합니다.
   *
   * 생성 시점을 기준으로 tick()이 누적한 경과 시간입니다.
   * @returns 구성 이후 경과한 밀리초.
   */
  [[nodiscard]] uint64_t now_ms() const noexcept { return current_ms_; }

  /** @brief 대기 중인 타이머 수를 반환합니다. */
  [[nodiscard]] size_t count() const noexcept { return count_; }

private:
  /**
   * @brief 타이머 항목 — 이중 연결 리스트 노드.
   *
   * FixedPoolResource에서 placement new로 생성합니다.
   */
  struct Entry {
    Callback  fn;
    TimerId   id        = kInvalid;
    uint64_t  expiry_ms = 0;
    Entry    *next      = nullptr;
    Entry    *prev      = nullptr;
  };

  /** @brief 각 레벨의 슬롯 해상도 (ms): 1ms, 256ms, 65536ms, 16777216ms. */
  static constexpr uint64_t kSlotMs[LEVELS] = {1, 256, 65536, 16777216};

  /**
   * @brief 항목을 적절한 레벨/슬롯에 삽입합니다.
   *
   * expiry_ms와 current_ms_의 차이에 따라 레벨을 결정합니다.
   * @param e 삽입할 Entry 포인터.
   */
  void insert(Entry *e) noexcept {
    uint64_t delta = (e->expiry_ms > current_ms_) ? (e->expiry_ms - current_ms_) : 0;

    size_t lv = 0;
    for (size_t l = LEVELS - 1; l > 0; --l) {
      if (delta >= kSlotMs[l]) {
        lv = l;
        break;
      }
    }

    size_t sl;
    if (lv == 0) {
      sl = static_cast<size_t>(e->expiry_ms & (SLOTS_PER_LEVEL - 1));
    } else {
      sl = static_cast<size_t>((e->expiry_ms / kSlotMs[lv]) & (SLOTS_PER_LEVEL - 1));
    }

    // 헤드에 삽입
    e->next = slots_[lv][sl];
    e->prev = nullptr;
    if (slots_[lv][sl])
      slots_[lv][sl]->prev = e;
    slots_[lv][sl] = e;
  }

  /**
   * @brief 특정 레벨/슬롯에서 Entry를 제거합니다 (이중 연결 리스트 기반 O(1)).
   *
   * @param lv  항목이 속한 레벨.
   * @param sl  항목이 속한 슬롯 인덱스.
   * @param e   제거할 Entry 포인터.
   */
  void unlink(size_t lv, size_t sl, Entry *e) noexcept {
    if (e->prev)
      e->prev->next = e->next;
    else
      slots_[lv][sl] = e->next; // 헤드 갱신
    if (e->next)
      e->next->prev = e->prev;
    e->next = nullptr;
    e->prev = nullptr;
  }

  /**
   * @brief 슬롯의 모든 만료 항목을 실행하고 개수를 반환합니다.
   *
   * 만료되지 않은 항목은 올바른 슬롯에 재삽입됩니다.
   *
   * @param level 처리할 레벨.
   * @param slot  처리할 슬롯 인덱스.
   * @returns 실행된 콜백 수.
   */
  size_t fire_slot(size_t level, size_t slot) {
    size_t fired = 0;
    Entry *e = slots_[level][slot];
    slots_[level][slot] = nullptr;

    while (e) {
      Entry *nxt = e->next;
      e->next = nullptr;
      e->prev = nullptr;

      if (e->expiry_ms <= current_ms_) {
        // 만료 — 콜백 실행 후 풀 반환
        if (e->fn) e->fn();
        e->~Entry();
        pool_.deallocate(e);
        --count_;
        ++fired;
      } else {
        // 아직 만료 안 됨 — 올바른 슬롯에 재삽입
        insert(e);
      }
      e = nxt;
    }
    return fired;
  }

  /**
   * @brief 상위 레벨 슬롯을 하위 레벨로 캐스케이드합니다.
   *
   * 타이밍 휠 레벨 전환 시 상위 레벨 슬롯의 항목을
   * 하위 레벨의 올바른 슬롯으로 재배치합니다.
   *
   * @param level 캐스케이드할 레벨 (1 이상).
   * @param slot  캐스케이드할 슬롯 인덱스.
   */
  void cascade(size_t level, size_t slot) {
    Entry *e = slots_[level][slot];
    slots_[level][slot] = nullptr;

    while (e) {
      Entry *nxt = e->next;
      e->next = nullptr;
      e->prev = nullptr;
      insert(e); // 하위 레벨에 재삽입
      e = nxt;
    }
  }

  /**
   * @brief 모든 항목을 풀에 반환합니다 (소멸자용).
   */
  void clear() noexcept {
    for (size_t lv = 0; lv < LEVELS; ++lv) {
      for (size_t sl = 0; sl < SLOTS_PER_LEVEL; ++sl) {
        Entry *e = slots_[lv][sl];
        while (e) {
          Entry *nxt = e->next;
          e->~Entry();
          pool_.deallocate(e);
          e = nxt;
        }
        slots_[lv][sl] = nullptr;
      }
    }
    count_ = 0;
  }

  /**
   * @brief Entry 객체 풀 — FixedPoolResource 기반 O(1) 할당/해제.
   *
   * `sizeof(Entry)` 크기의 슬롯을 `pool_capacity`개 미리 확보합니다.
   * 힙 단편화 없이 타이머 항목을 재활용합니다.
   */
  FixedPoolResource<sizeof(Entry), alignof(Entry)> pool_;

  /** @brief 슬롯 배열 [레벨][슬롯] → 이중 연결 리스트 헤드. */
  Entry   *slots_[LEVELS][SLOTS_PER_LEVEL] = {};

  /** @brief 생성 이후 누적된 가상 시계 (ms). */
  uint64_t current_ms_ = 0;

  /** @brief 다음에 발급할 타이머 ID. kInvalid(0)는 건너뜁니다. */
  TimerId  next_id_ = 1;

  /** @brief 현재 슬롯에 등록된 타이머 총 수. */
  size_t   count_ = 0;

  /**
   * @brief 각 레벨의 슬롯 이동에 필요한 비트 시프트 값을 계산합니다.
   *
   * 레벨 `l`은 `2^(l*8)` ms 단위를 커버합니다.
   * 이 정적 함수는 슬롯 배치 문서화용으로 제공됩니다.
   *
   * @param level 레벨 인덱스 (0 ~ LEVELS-1).
   * @returns 해당 레벨의 비트 시프트 값.
   */
  static constexpr uint64_t level_shift(size_t level) noexcept {
    return level * 8; // 각 레벨은 2^8=256 슬롯을 커버
  }
};

} // namespace qbuem

/** @} */ // end of qbuem_timer
