/**
 * @file numa_hugepages_example.cpp
 * @brief NUMA 스케줄링 + Huge Pages + CPU 힌트 + I/O 슬라이스 예제.
 *
 * ## 커버리지
 * - pin_reactor_to_cpu()        — Reactor를 특정 CPU에 고정
 * - auto_numa_bind()            — NUMA 노드별 Reactor 자동 배치
 * - numa_node_count()           — NUMA 노드 수 감지
 * - cpu_to_numa_map()           — CPU → NUMA 노드 매핑
 * - PerfCounters                — PMU 하드웨어 성능 카운터
 * - HugeBufferPool<N, Count>    — huge page mmap 버퍼 풀 (acquire/release)
 * - prefetch_read<Locality>()   — 읽기 prefetch
 * - prefetch_write<Locality>()  — 쓰기 prefetch
 * - prefetch_ahead<T, Ahead>()  — 연결 구조체 배열 prefetch
 * - kCacheLineSize              — 캐시 라인 크기 상수
 * - compiler_barrier()          — 컴파일러 메모리 배리어
 * - cpu_pause()                 — spin-wait pause 힌트
 * - IOSlice                     — 읽기 전용 scatter-gather 슬라이스
 * - MutableIOSlice              — 쓰기 가능 scatter-gather 슬라이스
 * - BufferPool<BufSize, Count>  — lock-free 범용 버퍼 풀
 */

#include <qbuem/core/cpu_hints.hpp>
#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/huge_pages.hpp>
#include <qbuem/core/numa.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/io/buffer_pool.hpp>
#include <qbuem/io/io_slice.hpp>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using namespace qbuem;
using namespace std::chrono_literals;

// ─────────────────────────────────────────────────────────────────────────────
// §1  CPU 힌트 — Prefetch & Cache-line
// ─────────────────────────────────────────────────────────────────────────────

struct alignas(kCacheLineSize) ReactorState {
    std::atomic<uint64_t> processed{0};
    std::atomic<uint64_t> errors{0};
    // 패딩: 다음 캐시라인과 false sharing 방지
    char _pad[kCacheLineSize - 2 * sizeof(std::atomic<uint64_t>)];
};

static void demo_cpu_hints() {
    std::printf("── §1  CPU 힌트 ──\n");
    std::printf("  캐시 라인 크기: %zu 바이트\n", kCacheLineSize);
    std::printf("  ReactorState 크기: %zu 바이트 (캐시라인 정렬)\n",
                sizeof(ReactorState));

    // 큰 배열의 순차 접근 시 prefetch로 TLB/캐시 미스 감소
    constexpr size_t N = 1024;
    std::vector<int> data(N, 0);

    for (size_t i = 0; i < N; ++i) {
        // 다음 접근 위치를 미리 캐시에 로드
        if (i + 8 < N)
            prefetch_read<3>(&data[i + 8]);
        data[i] = static_cast<int>(i * 2);
    }

    // 쓰기 prefetch: 출력 버퍼 준비
    std::vector<int> out(N);
    for (size_t i = 0; i < N; ++i) {
        if (i + 4 < N)
            prefetch_write<1>(&out[i + 4]);
        out[i] = data[i] + 1;
    }

    // prefetch_ahead: 연결 구조체 배열에서 N개 앞 prefetch
    prefetch_ahead(data.data(), 0, N);
    std::printf("  prefetch_ahead() 완료\n");

    // 컴파일러 배리어 & CPU pause
    compiler_barrier();
    cpu_pause();
    std::printf("  compiler_barrier(), cpu_pause() 완료\n");

    std::printf("  prefetch 완료: data[0]=%d, out[0]=%d\n\n", data[0], out[0]);
}

// ─────────────────────────────────────────────────────────────────────────────
// §2  Huge Pages 버퍼 풀
// ─────────────────────────────────────────────────────────────────────────────

static void demo_huge_pages() {
    std::printf("── §2  Huge Pages 버퍼 풀 ──\n");

    // 2 MiB 버퍼 4개를 가진 풀 (MAP_HUGETLB 시도, 실패 시 MAP_ANONYMOUS 폴백)
    HugeBufferPool<2 * 1024 * 1024, 4> pool;

    // 버퍼 1: 획득 + 사용 + 반납
    auto buf1 = pool.acquire();
    if (!buf1.empty()) {
        std::printf("  버퍼 1 획득: %zu 바이트\n", buf1.size());
        std::memset(buf1.data(), 0xAB, 64);  // 앞 64바이트 초기화
        std::printf("  버퍼 1 [0]=%02x (기대: ab)\n",
                    static_cast<unsigned char>(buf1[0]));
    } else {
        std::printf("  버퍼 1 획득 실패 (풀 고갈)\n");
    }

    // 버퍼 2: 획득
    auto buf2 = pool.acquire();
    if (!buf2.empty())
        std::printf("  버퍼 2 획득: %zu 바이트\n", buf2.size());

    // 풀 잔여량 확인 (4 - 2 = 2개 남음)
    std::printf("  풀 잔여: %zu / 4 버퍼\n", pool.available());

    // 반납
    pool.release(buf1);
    pool.release(buf2);
    std::printf("  버퍼 반납 후 잔여: %zu / 4 버퍼\n\n", pool.available());
}

// ─────────────────────────────────────────────────────────────────────────────
// §3  NUMA 친화성 설정 + PerfCounters
// ─────────────────────────────────────────────────────────────────────────────

static void demo_numa(Dispatcher& disp) {
    std::printf("── §3  NUMA 친화성 ──\n");

    // NUMA 노드 수 감지
    int nodes = numa_node_count();
    std::printf("  NUMA 노드 수: %d\n", nodes);

    // CPU → NUMA 매핑
    auto cpu_map = cpu_to_numa_map();
    std::printf("  논리 CPU 수: %zu\n", cpu_map.size());
    if (!cpu_map.empty())
        std::printf("  CPU 0 → NUMA 노드 %d\n", cpu_map[0]);

    // CPU 0에 첫 번째 Reactor 고정 시도
    bool pinned = pin_reactor_to_cpu(disp, 0, 0);
    std::printf("  Reactor[0] → CPU 0 고정: %s\n",
                pinned ? "성공" : "지원 안 됨 (비Linux 또는 범위 초과)");

    // NUMA 노드별 자동 배치
    size_t bound = auto_numa_bind(disp);
    std::printf("  auto_numa_bind() 바인딩: %zu 워커\n", bound);

    // PerfCounters — PMU 하드웨어 성능 카운터
    std::printf("\n── §3b  PerfCounters ──\n");
    PerfCounters perf;
    std::printf("  PMU 카운터 사용 가능: %s\n",
                perf.available() ? "yes (CAP_PERFMON 필요)" : "no (권한/하드웨어 없음)");

    perf.start();
    // 측정할 작업
    volatile uint64_t sum = 0;
    for (int i = 0; i < 10000; ++i) sum += static_cast<uint64_t>(i);
    auto snap = perf.stop();

    std::printf("  cycles: %llu, instructions: %llu\n",
                static_cast<unsigned long long>(snap.cycles),
                static_cast<unsigned long long>(snap.instructions));
    std::printf("  IPC: %.2f\n", snap.ipc());
    std::printf("  LLC misses: %llu, branch misses: %llu\n\n",
                static_cast<unsigned long long>(snap.llc_misses),
                static_cast<unsigned long long>(snap.branch_misses));
}

// ─────────────────────────────────────────────────────────────────────────────
// §4  I/O 슬라이스 & 버퍼 풀
// ─────────────────────────────────────────────────────────────────────────────

static void demo_io_slice() {
    std::printf("── §4  I/O 슬라이스 & 범용 버퍼 풀 ──\n");

    // IOSlice — 읽기 전용 scatter-gather 슬라이스
    std::string part1 = "Hello, ";
    std::string part2 = "qbuem!";
    std::vector<IOSlice> slices = {
        IOSlice{reinterpret_cast<const std::byte*>(part1.data()), part1.size()},
        IOSlice{reinterpret_cast<const std::byte*>(part2.data()), part2.size()},
    };
    size_t total = 0;
    for (auto& s : slices) total += s.size;
    std::printf("  IOSlice 2개, 총 %zu 바이트\n", total);

    // to_buffer_view() / to_iovec() 변환
    auto bv   = slices[0].to_buffer_view();
    auto iov0 = slices[0].to_iovec();
    std::printf("  IOSlice[0] BufferView 크기: %zu, iovec 크기: %zu\n",
                bv.size(), iov0.iov_len);

    // MutableIOSlice — 쓰기 가능 슬라이스
    alignas(64) std::byte buf1[16]{};
    alignas(64) std::byte buf2[16]{};
    std::vector<MutableIOSlice> mut_slices = {
        MutableIOSlice{buf1, sizeof(buf1)},
        MutableIOSlice{buf2, sizeof(buf2)},
    };
    // 첫 번째 슬라이스에 데이터 복사
    std::memcpy(mut_slices[0].data, "QBUEM", 5);
    std::printf("  MutableIOSlice[0]: \"%s\"\n",
                reinterpret_cast<const char*>(buf1));

    // as_const() — MutableIOSlice → IOSlice 변환
    IOSlice ro = mut_slices[0].as_const();
    std::printf("  as_const() 크기: %zu\n", ro.size);

    // to_iovec() on MutableIOSlice
    auto iov1 = mut_slices[1].to_iovec();
    std::printf("  MutableIOSlice[1] iovec 크기: %zu\n", iov1.iov_len);

    // BufferPool<BufSize, Count> — lock-free 범용 버퍼 풀 (2개의 템플릿 인자)
    BufferPool<1024, 8> bp;   // 1 KiB 버퍼 8개
    std::printf("  BufferPool 가용: %zu / 8\n", bp.available());

    auto b = bp.acquire();
    if (b) {
        std::printf("  BufferPool 버퍼 획득 성공\n");
        b->data[0] = std::byte{0xFF};
        b->release();   // 풀에 자동 반납
        std::printf("  BufferPool 버퍼 반납 완료\n");
    }
    std::printf("  BufferPool 가용 (반납 후): %zu / 8\n\n", bp.available());
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::printf("=== qbuem NUMA + Huge Pages + CPU 힌트 예제 ===\n\n");

    demo_cpu_hints();
    demo_huge_pages();

    // Dispatcher 생성 후 NUMA 바인딩 데모
    Dispatcher disp(2);
    std::jthread t([&] { disp.run(); });

    demo_numa(disp);
    demo_io_slice();

    disp.stop();
    t.join();

    std::printf("=== 완료 ===\n");
    return 0;
}
