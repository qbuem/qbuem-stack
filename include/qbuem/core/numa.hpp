#pragma once

/**
 * @file qbuem/core/numa.hpp
 * @brief NUMA-aware scheduling and CPU affinity utilities
 * @defgroup qbuem_numa NUMA Scheduling
 * @ingroup qbuem_core
 *
 * Pins worker threads of a `Dispatcher` to specific CPU cores or NUMA nodes.
 *
 * ## Design
 * - `pin_reactor_to_cpu(dispatcher, idx, cpu_id)` — pthread_setaffinity_np
 * - `auto_numa_bind(dispatcher)` — automatically places reactor groups per NUMA node
 * - `NearestNumaAllocator` — NUMA-local memory allocation via mbind(2)
 *
 * ## Platform Support
 * - Linux: `pthread_setaffinity_np`, `mbind(2)`, `/sys/devices/system/node`
 * - Non-Linux: all functions are no-ops (graceful fallback)
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
// CPU affinity
// ---------------------------------------------------------------------------

/**
 * @brief Pin a specific Reactor worker thread to the given CPU core.
 *
 * Uses `pthread_setaffinity_np` to bind the worker at `reactor_idx` to
 * `cpu_id`. After this call the OS scheduler will not migrate that thread to
 * another core.
 *
 * @param dispatcher  Target Dispatcher.
 * @param reactor_idx Index of the worker to pin (0 ~ thread_count()-1).
 * @param cpu_id      Target CPU core number (0-based, logical CPU).
 * @returns true on success. false if the index is out of range or on
 *          non-Linux platforms.
 */
inline bool pin_reactor_to_cpu(Dispatcher &dispatcher,
                                size_t reactor_idx,
                                int cpu_id) noexcept {
#if defined(__linux__)
  if (reactor_idx >= dispatcher.thread_count()) return false;

  // Dispatcher does not expose internal thread handles, so we use post()
  // to let each worker set its own affinity from within its own thread.
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
// NUMA node detection
// ---------------------------------------------------------------------------

/**
 * @brief Return the number of NUMA nodes in the system.
 *
 * Linux: parses `/sys/devices/system/node/online`.
 * Non-Linux: always returns 1.
 *
 * @returns Number of NUMA nodes (minimum 1).
 */
inline int numa_node_count() noexcept {
#if defined(__linux__)
  std::ifstream f("/sys/devices/system/node/online");
  if (!f.is_open()) return 1;

  std::string line;
  std::getline(f, line);
  // Format examples: "0-3", "0,2", or "0"
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
 * @brief Return a mapping from CPU ID to NUMA node.
 *
 * Linux: parses `/sys/devices/system/node/nodeN/cpulist`.
 *
 * @returns cpu_to_node vector (index = logical CPU, value = NUMA node number).
 *          On non-Linux platforms, returns a vector of all zeros.
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
    // Simple parsing for "0-3,8-11" format
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
// Automatic NUMA binding
// ---------------------------------------------------------------------------

/**
 * @brief Evenly distribute Reactor workers across NUMA nodes.
 *
 * Divides `thread_count()` workers across the available NUMA nodes and pins
 * each worker to the first CPU of its assigned node.
 *
 * Example (4 workers, 2 NUMA nodes):
 * - Workers 0, 1 → CPUs on NUMA node 0
 * - Workers 2, 3 → CPUs on NUMA node 1
 *
 * @param dispatcher Dispatcher to bind.
 * @returns Number of workers actually bound. 0 on non-Linux platforms.
 */
inline size_t auto_numa_bind(Dispatcher &dispatcher) {
#if defined(__linux__)
  int nodes = numa_node_count();
  int num_cpus = static_cast<int>(std::thread::hardware_concurrency());
  auto node_map = cpu_to_numa_map();

  // Collect the list of CPUs per node
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
// PerfCounters — PMU events (Linux perf_event_open)
// ---------------------------------------------------------------------------

/**
 * @brief CPU hardware performance counter (PMU) wrapper.
 *
 * Uses Linux `perf_event_open(2)` to measure the following events:
 * - cycles        — CPU clock cycles
 * - instructions  — instructions retired
 * - llc_misses    — L3 cache misses
 * - branch_misses — branch mispredictions
 *
 * Degrades gracefully (values = 0) when permissions are insufficient or on
 * non-Linux platforms.
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

  /// @brief Enable the counters.
  void start() noexcept {
#if defined(__linux__) && defined(PERF_TYPE_HARDWARE)
    for (int fd : fds_) {
      if (fd >= 0) ::ioctl(fd, PERF_EVENT_IOC_RESET, 0);
      if (fd >= 0) ::ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
    }
#endif
  }

  /// @brief Disable the counters and return a snapshot of the current values.
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

  /// @brief Returns true if PMU counters were successfully opened.
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
    // Confirm perf_event_open availability via header inclusion
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
