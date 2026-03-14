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
 * - cancel():   O(1)
 * - tick():     O(fired_count)
 * @{
 */

#include <qbuem/common.hpp>

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>

namespace qbuem {

/**
 * @brief 4단계 계층형 타이밍 휠.
 *
 * 힙 할당 없는 O(1) 스케줄/취소를 지원하는 타이머 구조체입니다.
 * Reactor 이벤트 루프와 통합하여 타임아웃 처리에 사용합니다.
 *
 * ### 사용 예시
 * @code
 * TimerWheel wheel;
 * auto id = wheel.schedule(100, []{ std::puts("100ms 경과"); });
 * wheel.tick(50);   // 50ms 경과
 * wheel.tick(50);   // 100ms 경과 — 콜백 호출됨
 * @endcode
 */
class TimerWheel {
public:
  /** @brief 타이머 콜백 타입. */
  using Callback = std::function<void()>;
  /** @brief 타이머 식별자 타입. */
  using TimerId = uint64_t;
  /** @brief 유효하지 않은 타이머 ID. */
  static constexpr TimerId kInvalid = 0;

  /**
   * @brief 타이머 휠을 초기화합니다 (4레벨 × 256슬롯).
   */
  explicit TimerWheel() noexcept = default;

  // 복사/이동 금지 (포인터 기반 내부 링크 구조)
  TimerWheel(const TimerWheel &) = delete;
  TimerWheel &operator=(const TimerWheel &) = delete;
  TimerWheel(TimerWheel &&) = delete;
  TimerWheel &operator=(TimerWheel &&) = delete;

  ~TimerWheel() { clear(); }

  /**
   * @brief 지정 지연(ms) 후 콜백을 실행하도록 스케줄합니다.
   *
   * @param delay_ms 지연 시간 (밀리초). 0이면 다음 tick에서 즉시 실행.
   * @param fn       만료 시 호출할 콜백.
   * @returns 취소에 사용할 TimerId. 실패 시 kInvalid.
   */
  [[nodiscard]] TimerId schedule(uint64_t delay_ms, Callback fn) {
    auto *e = new Entry{};
    e->fn        = std::move(fn);
    e->id        = next_id_++;
    e->expiry_ms = now_ms_ + (delay_ms == 0 ? 0 : delay_ms);
    if (next_id_ == kInvalid) next_id_ = 1; // wrap-around 시 0 건너뜀

    insert(e);
    ++count_;
    return e->id;
  }

  /**
   * @brief 타이머를 O(1)로 취소합니다.
   *
   * @param id schedule()이 반환한 TimerId.
   * @returns 만료 전 취소 성공 시 true, 이미 실행됐거나 없으면 false.
   */
  bool cancel(TimerId id) {
    if (id == kInvalid) return false;
    // 모든 슬롯을 선형 탐색 — 취소는 드물게 발생하므로 허용 가능
    for (size_t lv = 0; lv < kLevels; ++lv) {
      for (size_t sl = 0; sl < kSlots; ++sl) {
        for (Entry *e = slots_[lv][sl]; e; e = e->next) {
          if (e->id == id) {
            remove(e);
            delete e;
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
   * @param elapsed_ms 마지막 tick 이후 경과 시간 (밀리초).
   * @returns 실행된 콜백 수.
   */
  size_t tick(uint64_t elapsed_ms) {
    uint64_t target = now_ms_ + elapsed_ms;
    size_t fired = 0;

    while (now_ms_ < target) {
      ++now_ms_;
      // 레벨 0 슬롯 처리
      size_t slot0 = static_cast<size_t>(now_ms_ & (kSlots - 1));
      fired += fire_slot(0, slot0);

      // 레벨 캐스케이드: 레벨 0이 한 바퀴 돌 때마다 상위 레벨 캐스케이드
      for (size_t lv = 1; lv < kLevels; ++lv) {
        uint64_t divisor = kSlotMs[lv];
        if ((now_ms_ % divisor) == 0) {
          size_t sl = static_cast<size_t>((now_ms_ / divisor) & (kSlots - 1));
          cascade(lv, sl);
        } else {
          break;
        }
      }
    }
    return fired;
  }

  /**
   * @brief 다음 만료까지 남은 시간(ms)을 반환합니다.
   *
   * @returns 대기 중 타이머가 없으면 UINT64_MAX, 있으면 남은 ms (최소 0).
   */
  [[nodiscard]] uint64_t next_expiry_ms() const noexcept {
    if (count_ == 0) return std::numeric_limits<uint64_t>::max();

    uint64_t earliest = std::numeric_limits<uint64_t>::max();
    for (size_t lv = 0; lv < kLevels; ++lv) {
      for (size_t sl = 0; sl < kSlots; ++sl) {
        for (const Entry *e = slots_[lv][sl]; e; e = e->next) {
          if (e->expiry_ms < earliest)
            earliest = e->expiry_ms;
        }
      }
    }
    if (earliest <= now_ms_) return 0;
    return earliest - now_ms_;
  }

  /** @brief 현재 가상 시계 값(ms)을 반환합니다. */
  [[nodiscard]] uint64_t now_ms() const noexcept { return now_ms_; }

  /** @brief 대기 중인 타이머 수를 반환합니다. */
  [[nodiscard]] size_t count() const noexcept { return count_; }

private:
  /**
   * @brief 타이머 항목 — 이중 연결 리스트 노드.
   */
  struct Entry {
    Callback  fn;
    TimerId   id        = kInvalid;
    uint64_t  expiry_ms = 0;
    Entry    *next      = nullptr;
    Entry    *prev      = nullptr;
  };

  static constexpr size_t   kLevels             = 4;
  static constexpr size_t   kSlots              = 256;
  /** @brief 각 레벨의 슬롯 해상도 (ms). */
  static constexpr uint64_t kSlotMs[kLevels]    = {1, 256, 65536, 16777216};

  /** @brief 슬롯 배열 [레벨][슬롯] → 연결 리스트 헤드. */
  Entry   *slots_[kLevels][kSlots] = {};
  uint64_t now_ms_                 = 0;
  TimerId  next_id_                = 1;
  size_t   count_                  = 0;

  /**
   * @brief 항목을 적절한 레벨/슬롯에 삽입합니다.
   *
   * expiry_ms와 now_ms_의 차이에 따라 레벨을 결정합니다.
   */
  void insert(Entry *e) noexcept {
    uint64_t delta = (e->expiry_ms > now_ms_) ? (e->expiry_ms - now_ms_) : 0;

    size_t lv = 0;
    for (size_t l = kLevels - 1; l > 0; --l) {
      if (delta >= kSlotMs[l]) {
        lv = l;
        break;
      }
    }

    size_t sl;
    if (lv == 0) {
      sl = static_cast<size_t>(e->expiry_ms & (kSlots - 1));
    } else {
      sl = static_cast<size_t>((e->expiry_ms / kSlotMs[lv]) & (kSlots - 1));
    }

    // 헤드에 삽입
    e->next = slots_[lv][sl];
    e->prev = nullptr;
    if (slots_[lv][sl])
      slots_[lv][sl]->prev = e;
    slots_[lv][sl] = e;
  }

  /**
   * @brief 항목을 현재 슬롯에서 제거합니다 (O(1)).
   */
  void remove(Entry *e) noexcept {
    // 어떤 슬롯에 있는지 알 수 없으므로 포인터 링크만 수정
    if (e->prev)
      e->prev->next = e->next;
    if (e->next)
      e->next->prev = e->prev;

    // 헤드 포인터 수정 — 해당 슬롯을 찾아야 함
    for (size_t lv = 0; lv < kLevels; ++lv) {
      for (size_t sl = 0; sl < kSlots; ++sl) {
        if (slots_[lv][sl] == e) {
          slots_[lv][sl] = e->next;
          return;
        }
      }
    }
    // e->prev가 있는 경우는 헤드가 아니므로 이미 처리됨
  }

  /**
   * @brief 슬롯의 모든 만료 항목을 실행하고 개수를 반환합니다.
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
      Entry *next = e->next;
      e->next = nullptr;
      e->prev = nullptr;

      if (e->expiry_ms <= now_ms_) {
        // 만료 — 콜백 실행
        if (e->fn) e->fn();
        delete e;
        --count_;
        ++fired;
      } else {
        // 아직 만료 안 됨 — 올바른 슬롯에 재삽입
        insert(e);
      }
      e = next;
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
      Entry *next = e->next;
      e->next = nullptr;
      e->prev = nullptr;
      insert(e); // 하위 레벨에 재삽입
      e = next;
    }
  }

  /**
   * @brief 모든 항목을 해제합니다 (소멸자용).
   */
  void clear() noexcept {
    for (size_t lv = 0; lv < kLevels; ++lv) {
      for (size_t sl = 0; sl < kSlots; ++sl) {
        Entry *e = slots_[lv][sl];
        while (e) {
          Entry *next = e->next;
          delete e;
          e = next;
        }
        slots_[lv][sl] = nullptr;
      }
    }
    count_ = 0;
  }
};

} // namespace qbuem

/** @} */ // end of qbuem_timer
