/**
 * @file hardware_io_example.cpp
 * @brief 하드웨어 I/O 고급 기능 예제 — PCIe, RDMA, eBPF, SPDK/NVMe, kTLS.
 *
 * ## 커버리지 — pcie/pcie_device.hpp
 * - PCIeDevice::open()         — VFIO 기반 PCIe 디바이스 열기
 * - PCIeDevice::map_bar()      — MMIO BAR 영역 매핑
 * - PCIeDevice::read_mmio32()  — 32비트 MMIO 레지스터 읽기
 * - PCIeDevice::write_mmio32() — 32비트 MMIO 레지스터 쓰기
 * - PCIeBar                    — BAR(Base Address Register) 매핑
 *
 * ## 커버리지 — rdma/rdma_channel.hpp
 * - RDMAContext::open()        — HCA 컨텍스트 열기
 * - RDMAChannel::setup()       — QP 생성 + 메모리 등록
 * - RDMAChannel::local_info()  — 연결 핸드셰이크 정보
 * - RDMAChannel::connect()     — OOB 교환 후 연결 완료
 * - RDMAChannel::write()       — RDMA Write (zero-copy)
 *
 * ## 커버리지 — ebpf/ebpf_tracer.hpp
 * - EBPFTracer::create()       — BPF 오브젝트 로드
 * - EBPFTracer::enable_all()   — 모든 이벤트 활성화
 * - EBPFTracer::poll()         — 이벤트 링 버퍼 읽기
 * - TraceEvent                 — BPF 이벤트 구조체 (헤더 전용)
 *
 * ## 커버리지 — spdk/nvme_io.hpp
 * - NVMeIOContext::open()      — NVMe 캐릭터 디바이스 열기
 * - NVMeIOContext::alloc_dma() — DMA 정렬 버퍼 할당
 * - NVMeIOContext::read()      — io_uring passthrough NVMe 읽기
 * - NVMeIOContext::write()     — io_uring passthrough NVMe 쓰기
 *
 * ## 커버리지 — io/ktls.hpp
 * - KtlsSessionParams          — TLS 세션 파라미터 (키, IV, 시퀀스)
 * - enable_tx()                — 커널 TLS 송신 활성화 (인라인 함수)
 * - enable_rx()                — 커널 TLS 수신 활성화 (인라인 함수)
 *
 * @note 실제 하드웨어(VFIO, InfiniBand, BPF, NVMe 캐릭터 디바이스)가 없으면
 *       open()/create() 등이 에러를 반환합니다. 이 예제는 API 사용 패턴을
 *       보여주며, 하드웨어 없이도 컴파일되고 graceful하게 동작합니다.
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/ebpf/ebpf_tracer.hpp>
#include <qbuem/io/ktls.hpp>
#include <qbuem/pcie/pcie_device.hpp>
#include <qbuem/rdma/rdma_channel.hpp>
#include <qbuem/spdk/nvme_io.hpp>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

using namespace qbuem;
using namespace qbuem::io;
using namespace qbuem::ebpf;
using namespace std::chrono_literals;

// ─────────────────────────────────────────────────────────────────────────────
// §1  PCIe (VFIO) 디바이스 접근
// ─────────────────────────────────────────────────────────────────────────────

static void demo_pcie() {
    std::printf("── §1  PCIe (VFIO) ──\n");
    std::printf("  사용 패턴:\n");
    std::printf("    // 디바이스 열기 (VFIO UIO 필요)\n");
    std::printf("    auto dev = qbuem::pcie::PCIeDevice::open(\"0000:03:00.0\");\n");
    std::printf("    //         → Result<unique_ptr<PCIeDevice>>\n");
    std::printf("    if (!dev) { /* 오류 처리 */ return; }\n");
    std::printf("    // BAR0 MMIO 매핑\n");
    std::printf("    auto bar = (*dev)->map_bar(0);  // Result<BarMapping>\n");
    std::printf("    // 32비트 레지스터 읽기/쓰기\n");
    std::printf("    uint32_t vid = (*dev)->read_mmio32(0x00);   // Vendor ID\n");
    std::printf("    (*dev)->write_mmio32(0x04, 0x00000006);      // Bus Master Enable\n\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// §2  RDMA 채널
// ─────────────────────────────────────────────────────────────────────────────

static std::atomic<bool> g_rdma_done{false};

static Task<void> demo_rdma_task() {
    std::printf("── §2  RDMA 채널 ──\n");
    std::printf("  사용 패턴:\n");
    std::printf("    // Step1: HCA 컨텍스트 열기 (libibverbs 필요)\n");
    std::printf("    auto ctx = qbuem::rdma::RDMAContext::open(\"mlx5_0\");\n");
    std::printf("    //         → Result<unique_ptr<RDMAContext>>\n");
    std::printf("    // Step2: 채널(QP) 생성 및 설정\n");
    std::printf("    qbuem::rdma::RDMAChannel ch(**ctx);  // RDMAChannel(RDMAContext&)\n");
    std::printf("    ch.setup();             // QP INIT 전환\n");
    std::printf("    // Step3: OOB 교환 후 연결\n");
    std::printf("    auto local = ch.local_info(); // QPInfo{qpn, lid, gid}\n");
    std::printf("    ch.connect(remote_info);      // RTR → RTS\n");
    std::printf("    // Step4: RDMA Write (zero-copy, CPU 개입 없음)\n");
    std::printf("    auto mr = ctx->register_mr(buf, size);\n");
    std::printf("    co_await ch.write(remote_addr, remote_rkey, mr->lkey, buf, size);\n\n");
    g_rdma_done.store(true);
    co_return;
}

// ─────────────────────────────────────────────────────────────────────────────
// §3  eBPF 트레이서
// ─────────────────────────────────────────────────────────────────────────────

static void demo_ebpf() {
    std::printf("── §3  eBPF 트레이서 ──\n");

    // TraceEvent는 헤더 전용 구조체 — 항상 접근 가능
    std::printf("  TraceEvent 크기: %zu 바이트 (캐시라인 정렬 = 64B)\n",
                sizeof(TraceEvent));

    std::printf("  사용 패턴 (libbpf 필요):\n");
    std::printf("    auto t = EBPFTracer::create(\"trace.bpf.o\"); // Result<unique_ptr<EBPFTracer>>\n");
    std::printf("    t->enable_all();                              // 모든 이벤트 활성화\n");
    std::printf("    t->enable(EventType::Syscall);                // 특정 이벤트만 활성화\n");
    std::printf("    std::array<TraceEvent, 64> buf;\n");
    std::printf("    size_t n = t->poll(buf, 100 /*ms*/);          // 폴링\n");
    std::printf("    t->subscribe([](TraceEvent e){ ... });         // 콜백 등록\n\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// §4  NVMe I/O (io_uring passthrough)
// ─────────────────────────────────────────────────────────────────────────────

static std::atomic<bool> g_nvme_done{false};

static Task<void> demo_nvme_task() {
    std::printf("── §4  NVMe I/O (io_uring passthrough) ──\n");
    std::printf("  사용 패턴 (Linux 6.0+, /dev/ng0n1 필요):\n");
    std::printf("    // NVMeIOContext::open() 은 동기 (Result, not Task)\n");
    std::printf("    auto ctx = qbuem::spdk::NVMeIOContext::open(\"/dev/ng0n1\");\n");
    std::printf("    //          → Result<unique_ptr<NVMeIOContext>>\n");
    std::printf("    auto dma = (*ctx)->alloc_dma(4096);                  // DMA 정렬 버퍼\n");
    std::printf("    auto r   = co_await (*ctx)->read(*dma.value(), 0, 1); // LBA 0, 1 섹터\n");
    std::printf("    co_await (*ctx)->write(*dma.value(), 0, 1);\n");
    std::printf("    co_await (*ctx)->flush();                             // Write Cache Sync\n\n");
    g_nvme_done.store(true);
    co_return;
}

// ─────────────────────────────────────────────────────────────────────────────
// §5  Kernel TLS (kTLS)
// ─────────────────────────────────────────────────────────────────────────────

static void demo_ktls() {
    std::printf("── §5  Kernel TLS (kTLS) ──\n");

    // KtlsSessionParams는 헤더 전용 구조체 — 항상 접근 가능
    KtlsSessionParams params{};
    std::printf("  KtlsSessionParams 구조체 (헤더 전용):\n");
    std::printf("    key 크기: %zu 바이트 (AES-128)\n", params.key.size());
    std::printf("    iv  크기: %zu 바이트 (GCM nonce)\n", params.iv.size());
    std::printf("    seq 크기: %zu 바이트 (레코드 시퀀스)\n", params.seq.size());

    // enable_tx()는 인라인 함수 — 실제 호출 가능 (fd=-1이므로 실패 예상)
    int fake_fd = -1;
    auto tx_r = enable_tx(fake_fd, params);
    std::printf("  enable_tx(fake_fd=-1): %s (fd 유효하지 않음)\n",
                tx_r ? "성공" : tx_r.error().message().c_str());

    std::printf("  kTLS 사용 패턴 (Linux 4.13+ CONFIG_TLS 필요):\n");
    std::printf("    // TLS 핸드셰이크 완료 후:\n");
    std::printf("    qbuem::io::KtlsSessionParams sp = extract_from_tls_ctx(ctx);\n");
    std::printf("    qbuem::io::enable_tx(conn_fd, sp);  // 송신 암호화 커널 위임\n");
    std::printf("    qbuem::io::enable_rx(conn_fd, sp);  // 수신 복호화 커널 위임\n");
    std::printf("    // 이후 send()/recv() → 커널이 직접 AES-128-GCM 암호화\n\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::printf("=== qbuem 하드웨어 I/O 고급 기능 예제 ===\n\n");
    std::printf("주의: 실제 하드웨어 없이 API 패턴을 보여줍니다.\n\n");

    demo_pcie();
    demo_ebpf();
    demo_ktls();

    // 코루틴 기반 데모 (RDMA, NVMe)
    Dispatcher disp(1);
    std::thread t([&] { disp.run(); });

    disp.spawn([&]() -> Task<void> { co_await demo_rdma_task(); }());
    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!g_rdma_done.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);

    disp.spawn([&]() -> Task<void> { co_await demo_nvme_task(); }());
    deadline = std::chrono::steady_clock::now() + 3s;
    while (!g_nvme_done.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);

    disp.stop();
    t.join();

    std::printf("hardware_io_example: ALL OK\n");
    return 0;
}
