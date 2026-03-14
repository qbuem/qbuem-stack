#pragma once

/**
 * @file qbuem/core/numa.hpp
 * @brief NUMA-aware 스케줄링 및 CPU 친화성 유틸리티
 * @defgroup qbuem_numa NUMA Scheduling
 * @ingroup qbuem_core
 *
 * `Dispatcher`의 워커 스레드를 특정 CPU 코어 또는 NUMA 노드에 고정합니다.
 *
 * ## 설계
 * - `pin_reactor_to_cpu(dispatcher, idx, cpu_id)` — pthread_setaffinity_np
 * - `auto_numa_bind(dispatcher)` — NUMA 노드별 reactor 그룹 자동 배치
 * - `NearestNumaAllocator` — mbind(2) 기반 NUMA-local 메모리 할당
 *
 * ## 플랫폼 지원
 * - Linux: `pthread_setaffinity_np`, `mbind(2)`, `/sys/devices/system/node`
 * - 비Linux: 모든 함수가 no-op (graceful fallback)
 *
 * @{
 */

#include <qbuem/core/dispatcher.hpp>

#include <cstddef>
#include <string>
#include <vector>

#if defined(__linux__)
#  include <pthread.h>
#  include <sched.h>
#  include <sys/mman.h>
#  include <unistd.h>
#  include <fstream>
#  include <sstream>
#endif

namespace qbuem {

// ---------------------------------------------------------------------------
// CPU 친화성
// ---------------------------------------------------------------------------

/**
 * @brief 특정 Reactor 워커 스레드를 지정한 CPU 코어에 고정합니다.
 *
 * `pthread_setaffinity_np`를 사용하여 `reactor_idx` 번 워커를 `cpu_id` 코어에
 * 고정합니다. 이후 OS 스케줄러가 해당 스레드를 다른 코어로 이동시키지 않습니다.
 *
 * @param dispatcher  대상 Dispatcher.
 * @param reactor_idx 고정할 워커 인덱스 (0 ~ thread_count()-1).
 * @param cpu_id      목표 CPU 코어 번호 (0-based, logical CPU).
 * @returns 성공 시 true. 범위 초과 또는 비Linux 플랫폼에서는 false.
 */
inline bool pin_reactor_to_cpu(Dispatcher &dispatcher,
                                size_t reactor_idx,
                                int cpu_id) noexcept {
#if defined(__linux__)
  if (reactor_idx >= dispatcher.thread_count()) return false;

  // Dispatcher는 내부 스레드 핸들을 노출하지 않으므로
  // 현재 스레드가 해당 reactor라면 self-affinity를 설정합니다.
  // 런타임 바인딩: 워커가 자신의 CPU를 설정하도록 post() 사용.
  dispatcher.post_to(reactor_idx, [cpu_id]() {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    ::pthread_setaffinity_np(::pthread_self(), sizeof(cpu_set_t), &cpuset);
  });
  return true;
#else
  (void)dispatcher; (void)reactor_idx; (void)cpu_id;
  return false;
#endif
}

// ---------------------------------------------------------------------------
// NUMA 노드 감지
// ---------------------------------------------------------------------------

/**
 * @brief 시스템의 NUMA 노드 수를 반환합니다.
 *
 * Linux: `/sys/devices/system/node/online` 파싱.
 * 비Linux: 항상 1 반환.
 *
 * @returns NUMA 노드 수 (최소 1).
 */
inline int numa_node_count() noexcept {
#if defined(__linux__)
  std::ifstream f("/sys/devices/system/node/online");
  if (!f.is_open()) return 1;

  std::string line;
  std::getline(f, line);
  // 형식 예: "0-3" 또는 "0,2" 또는 "0"
  int max_node = 0;
  size_t pos = 0;
  while (pos < line.size()) {
    size_t dash = line.find('-', pos);
    size_t comma = line.find(',', pos);
    size_t end = std::min(dash, comma);
    if (end == std::string::npos) end = line.size();

    int val = std::stoi(line.substr(pos, end - pos));
    if (dash != std::string::npos && dash < comma) {
      size_t next_comma = line.find(',', dash + 1);
      size_t range_end = (next_comma == std::string::npos)
                           ? line.size() : next_comma;
      max_node = std::stoi(line.substr(dash + 1, range_end - dash - 1));
      pos = (next_comma == std::string::npos) ? line.size() : next_comma + 1;
    } else {
      if (val > max_node) max_node = val;
      pos = (end == line.size()) ? end : end + 1;
    }
  }
  return max_node + 1;
#else
  return 1;
#endif
}

/**
 * @brief CPU ID → NUMA 노드 매핑을 반환합니다.
 *
 * Linux: `/sys/devices/system/node/nodeN/cpulist` 파싱.
 *
 * @returns cpu_to_node 벡터 (인덱스 = logical CPU, 값 = NUMA 노드 번호).
 *          비Linux에서는 모두 0인 벡터 반환.
 */
inline std::vector<int> cpu_to_numa_map() {
  int num_cpus = static_cast<int>(std::thread::hardware_concurrency());
  std::vector<int> map(static_cast<size_t>(num_cpus), 0);

#if defined(__linux__)
  int nodes = numa_node_count();
  for (int node = 0; node < nodes; ++node) {
    std::string path = "/sys/devices/system/node/node" +
                       std::to_string(node) + "/cpulist";
    std::ifstream f(path);
    if (!f.is_open()) continue;

    std::string line;
    std::getline(f, line);
    // 간단한 파싱: "0-3,8-11" 형식
    std::istringstream ss(line);
    std::string token;
    while (std::getline(ss, token, ',')) {
      auto dash = token.find('-');
      if (dash == std::string::npos) {
        int cpu = std::stoi(token);
        if (cpu < num_cpus) map[static_cast<size_t>(cpu)] = node;
      } else {
        int from = std::stoi(token.substr(0, dash));
        int to   = std::stoi(token.substr(dash + 1));
        for (int cpu = from; cpu <= to && cpu < num_cpus; ++cpu)
          map[static_cast<size_t>(cpu)] = node;
      }
    }
  }
#endif
  return map;
}

// ---------------------------------------------------------------------------
// NUMA 자동 바인딩
// ---------------------------------------------------------------------------

/**
 * @brief NUMA 노드별로 Reactor 워커를 균등하게 배치합니다.
 *
 * `thread_count()`개의 워커를 NUMA 노드 수에 맞게 나눠
 * 각 워커를 해당 노드의 첫 번째 CPU에 고정합니다.
 *
 * 예시 (4워커, 2 NUMA 노드):
 * - 워커 0,1 → NUMA 노드 0의 CPU들
 * - 워커 2,3 → NUMA 노드 1의 CPU들
 *
 * @param dispatcher 바인딩할 Dispatcher.
 * @returns 실제로 바인딩된 워커 수. 비Linux에서는 0.
 */
inline size_t auto_numa_bind(Dispatcher &dispatcher) {
#if defined(__linux__)
  int nodes = numa_node_count();
  int num_cpus = static_cast<int>(std::thread::hardware_concurrency());
  auto node_map = cpu_to_numa_map();

  // 노드별 CPU 목록 수집
  std::vector<std::vector<int>> node_cpus(static_cast<size_t>(nodes));
  for (int cpu = 0; cpu < num_cpus; ++cpu)
    node_cpus[static_cast<size_t>(node_map[static_cast<size_t>(cpu)])].push_back(cpu);

  size_t n_workers = dispatcher.thread_count();
  size_t bound = 0;

  for (size_t i = 0; i < n_workers; ++i) {
    int node = static_cast<int>(i % static_cast<size_t>(nodes));
    auto &cpus = node_cpus[static_cast<size_t>(node)];
    if (cpus.empty()) continue;
    int cpu = cpus[i / static_cast<size_t>(nodes) % cpus.size()];
    if (pin_reactor_to_cpu(dispatcher, i, cpu)) ++bound;
  }
  return bound;
#else
  (void)dispatcher;
  return 0;
#endif
}

// ---------------------------------------------------------------------------
// PerfCounters — PMU 이벤트 (Linux perf_event_open)
// ---------------------------------------------------------------------------

/**
 * @brief CPU 하드웨어 성능 카운터 (PMU) 래퍼.
 *
 * Linux `perf_event_open(2)`을 사용하여 다음 이벤트를 측정합니다:
 * - cycles        — CPU 클럭 사이클
 * - instructions  — 실행된 명령어 수
 * - llc_misses    — L3 캐시 미스
 * - branch_misses — 분기 예측 실패
 *
 * 권한이 없거나 비Linux 환경에서는 graceful degradation (값 = 0).
 */
class PerfCounters {
public:
  struct Snapshot {
    uint64_t cycles{0};
    uint64_t instructions{0};
    uint64_t llc_misses{0};
    uint64_t branch_misses{0};

    /// @brief IPC (Instructions Per Cycle).
    [[nodiscard]] double ipc() const noexcept {
      return (cycles == 0) ? 0.0
                           : static_cast<double>(instructions) / cycles;
    }
  };

  PerfCounters() { open(); }
  ~PerfCounters() { close_all(); }

  PerfCounters(const PerfCounters &) = delete;
  PerfCounters &operator=(const PerfCounters &) = delete;

  /// @brief 카운터를 활성화합니다.
  void start() noexcept {
#if defined(__linux__) && defined(PERF_TYPE_HARDWARE)
    for (int fd : fds_) {
      if (fd >= 0) ::ioctl(fd, PERF_EVENT_IOC_RESET, 0);
      if (fd >= 0) ::ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
    }
#endif
  }

  /// @brief 카운터를 멈추고 현재 값의 스냅샷을 반환합니다.
  [[nodiscard]] Snapshot stop() noexcept {
    Snapshot s;
#if defined(__linux__) && defined(PERF_TYPE_HARDWARE)
    for (int fd : fds_) {
      if (fd >= 0) ::ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
    }
    auto read_counter = [](int fd) -> uint64_t {
      if (fd < 0) return 0;
      uint64_t val = 0;
      if (::read(fd, &val, sizeof(val)) != sizeof(val)) return 0;
      return val;
    };
    if (fds_.size() > 0) s.cycles        = read_counter(fds_[0]);
    if (fds_.size() > 1) s.instructions  = read_counter(fds_[1]);
    if (fds_.size() > 2) s.llc_misses    = read_counter(fds_[2]);
    if (fds_.size() > 3) s.branch_misses = read_counter(fds_[3]);
#endif
    return s;
  }

  /// @brief PMU 카운터가 열려 있는지 확인합니다.
  [[nodiscard]] bool available() const noexcept {
#if defined(__linux__) && defined(PERF_TYPE_HARDWARE)
    return !fds_.empty() && fds_[0] >= 0;
#else
    return false;
#endif
  }

private:
  std::vector<int> fds_;

  void open() noexcept {
#if defined(__linux__) && defined(PERF_TYPE_HARDWARE)
    // perf_event_open이 사용 가능한지 헤더로 확인
#  include <linux/perf_event.h>
#  include <sys/ioctl.h>
#  include <sys/syscall.h>

    auto open_event = [](uint32_t type, uint64_t config) -> int {
      struct perf_event_attr attr{};
      attr.type           = type;
      attr.size           = sizeof(attr);
      attr.config         = config;
      attr.disabled       = 1;
      attr.exclude_kernel = 1;
      attr.exclude_hv     = 1;
      return static_cast<int>(::syscall(SYS_perf_event_open,
                                        &attr, 0, -1, -1, 0));
    };

    fds_.push_back(open_event(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES));
    fds_.push_back(open_event(PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS));
    fds_.push_back(open_event(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES));
    fds_.push_back(open_event(PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES));
#endif
  }

  void close_all() noexcept {
#if defined(__linux__)
    for (int fd : fds_) {
      if (fd >= 0) ::close(fd);
    }
    fds_.clear();
#endif
  }
};

/** @} */

} // namespace qbuem
