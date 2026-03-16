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
 * - RDMAChannel::create()      — QP 생성 + 메모리 등록
 * - RDMAChannel::local_info()  — 연결 핸드셰이크 정보
 * - RDMAChannel::connect()     — OOB 교환 후 연결 완료
 * - RDMAChannel::write()       — RDMA Write (zero-copy)
 * - RDMAChannel::send()        — Send/Recv (양측 CPU 개입)
 *
 * ## 커버리지 — ebpf/ebpf_tracer.hpp
 * - EBPFTracer::create()       — BPF 오브젝트 로드
 * - EBPFTracer::attach_kprobe()— kprobe 부착
 * - EBPFTracer::read_events()  — 이벤트 링 버퍼 읽기
 * - TraceEvent                 — BPF 이벤트 구조체
 * - BpfMapStats                — BPF 맵 통계
 *
 * ## 커버리지 — spdk/nvme_io.hpp
 * - NVMeIO::open()             — NVMe 캐릭터 디바이스 열기
 * - NVMeIO::read()             — io_uring passthrough NVMe 읽기
 * - NVMeIO::write()            — io_uring passthrough NVMe 쓰기
 * - NVMeIO::identify()         — NVMe Identify Command
 *
 * ## 커버리지 — io/ktls.hpp
 * - ktls::TlsParams            — TLS 세션 파라미터 (키, IV, 시퀀스)
 * - ktls::enable_tx()          — 커널 TLS 송신 활성화
 * - ktls::enable_rx()          — 커널 TLS 수신 활성화
 * - ktls::is_supported()       — kTLS 커널 지원 여부
 * - ktls::sendfile_tls()       — kTLS + sendfile zero-copy 전송
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
using namespace std::chrono_literals;

// ─────────────────────────────────────────────────────────────────────────────
// §1  PCIe (VFIO) 디바이스 접근
// ─────────────────────────────────────────────────────────────────────────────

static void demo_pcie() {
    std::printf("── §1  PCIe (VFIO) ──\n");

    // 실제 PCIe 디바이스가 없으면 open()이 실패합니다
    auto dev = PCIeDevice::open("0000:03:00.0");
    if (!dev) {
        std::printf("  PCIeDevice::open(): %s\n",
                    dev.error().message().c_str());
        std::printf("  (VFIO 드라이버/실제 하드웨어 필요 — API 패턴 표시)\n");
        std::printf("  사용 패턴:\n");
        std::printf("    auto dev = PCIeDevice::open(\"0000:03:00.0\");\n");
        std::printf("    auto bar = dev->map_bar(0);\n");
        std::printf("    uint32_t id = dev->read_mmio32(*bar, 0x00);\n");
        std::printf("    dev->write_mmio32(*bar, 0x04, 0x1);\n\n");
        return;
    }

    // BAR0 매핑
    auto bar = dev->map_bar(0);
    if (!bar) {
        std::printf("  map_bar(0) 실패: %s\n", bar.error().message().c_str());
        return;
    }

    // MMIO 레지스터 읽기/쓰기
    uint32_t vendor_id = dev->read_mmio32(*bar, 0x00);
    std::printf("  Vendor ID (offset 0x00): 0x%08x\n", vendor_id);
    dev->write_mmio32(*bar, 0x04, 0x00000006);  // Bus Master Enable
    std::printf("  BAR0 매핑 크기: %zu 바이트\n\n", bar->size);
}

// ─────────────────────────────────────────────────────────────────────────────
// §2  RDMA 채널
// ─────────────────────────────────────────────────────────────────────────────

static std::atomic<bool> g_rdma_done{false};

static Task<void> demo_rdma_task() {
    std::printf("── §2  RDMA 채널 ──\n");

    // RDMA 채널 생성 (IB 디바이스 없으면 실패)
    auto ch_r = co_await RDMAChannel::create(
        RDMAChannel::Config{
            .device    = "mlx5_0",  // 실제 IB 디바이스 이름
            .port      = 1,
            .gid_index = 0,
            .buf_size  = 4096,
        });

    if (!ch_r) {
        std::printf("  RDMAChannel::create(): %s\n",
                    ch_r.error().message().c_str());
        std::printf("  (InfiniBand/RoCE 하드웨어 필요 — API 패턴 표시)\n");
        std::printf("  사용 패턴:\n");
        std::printf("    auto ch = co_await RDMAChannel::create(cfg);\n");
        std::printf("    auto info = ch->local_info();\n");
        std::printf("    // OOB 교환 후:\n");
        std::printf("    co_await ch->connect(remote_info);\n");
        std::printf("    co_await ch->write(data, remote_addr, rkey);\n\n");
        g_rdma_done.store(true);
        co_return;
    }

    // 로컬 연결 정보 출력
    auto info = ch_r->local_info();
    std::printf("  로컬 QPN: %u, LID: %u\n", info.qpn, info.lid);

    g_rdma_done.store(true);
    co_return;
}

// ─────────────────────────────────────────────────────────────────────────────
// §3  eBPF 트레이서
// ─────────────────────────────────────────────────────────────────────────────

static void demo_ebpf() {
    std::printf("── §3  eBPF 트레이서 ──\n");

    // BPF 오브젝트 로드 (파일 없으면 실패)
    auto tracer = EBPFTracer::create("qbuem_trace.bpf.o");
    if (!tracer) {
        std::printf("  EBPFTracer::create(): %s\n",
                    tracer.error().message().c_str());
        std::printf("  (BPF 오브젝트 파일/CAP_BPF 필요 — API 패턴 표시)\n");
        std::printf("  사용 패턴:\n");
        std::printf("    auto t = EBPFTracer::create(\"trace.bpf.o\");\n");
        std::printf("    t->attach_kprobe(\"tcp_sendmsg\");\n");
        std::printf("    auto events = t->read_events(100ms);\n");

        // TraceEvent 구조체 크기 검증
        std::printf("  TraceEvent 크기: %zu 바이트 (캐시라인 = 64B)\n\n",
                    sizeof(TraceEvent));
        return;
    }

    // kprobe 부착
    auto attach_r = tracer.value()->attach_kprobe("sys_write");
    if (attach_r)
        std::printf("  kprobe(sys_write) 부착 완료\n");

    // 이벤트 읽기 (100ms 폴링)
    auto events = tracer.value()->read_events(100ms);
    std::printf("  읽은 이벤트 수: %zu\n\n", events.size());
}

// ─────────────────────────────────────────────────────────────────────────────
// §4  NVMe I/O (io_uring passthrough)
// ─────────────────────────────────────────────────────────────────────────────

static std::atomic<bool> g_nvme_done{false};

static Task<void> demo_nvme_task() {
    std::printf("── §4  NVMe I/O (io_uring passthrough) ──\n");

    // NVMe 캐릭터 디바이스 열기 (/dev/ng0n1 = 첫 번째 NVMe 네임스페이스)
    auto nvme_r = co_await NVMeIO::open("/dev/ng0n1");
    if (!nvme_r) {
        std::printf("  NVMeIO::open(): %s\n",
                    nvme_r.error().message().c_str());
        std::printf("  (NVMe 캐릭터 디바이스/Linux 6.0+ 필요 — API 패턴 표시)\n");
        std::printf("  사용 패턴:\n");
        std::printf("    auto io = co_await NVMeIO::open(\"/dev/ng0n1\");\n");
        std::printf("    auto id = co_await io->identify();\n");
        std::printf("    std::vector<std::byte> buf(4096);\n");
        std::printf("    co_await io->read(buf, /*lba=*/0);\n");
        std::printf("    co_await io->write(buf, /*lba=*/0);\n\n");
        g_nvme_done.store(true);
        co_return;
    }

    // NVMe Identify (디바이스 정보)
    auto id_r = co_await nvme_r.value()->identify();
    if (id_r) {
        std::printf("  NVMe 모델: %.40s\n", id_r->model_number.c_str());
        std::printf("  시리얼:    %.20s\n", id_r->serial_number.c_str());
        std::printf("  펌웨어:    %.8s\n",  id_r->firmware_rev.c_str());
    }

    // 4 KiB 읽기
    std::vector<std::byte> buf(4096);
    auto read_r = co_await nvme_r.value()->read(buf, 0);
    if (read_r)
        std::printf("  LBA 0 읽기: %zu 바이트\n", *read_r);

    g_nvme_done.store(true);
    co_return;
}

// ─────────────────────────────────────────────────────────────────────────────
// §5  Kernel TLS (kTLS)
// ─────────────────────────────────────────────────────────────────────────────

static void demo_ktls() {
    std::printf("── §5  Kernel TLS (kTLS) ──\n");

    // kTLS 지원 여부 감지
    bool supported = ktls::is_supported();
    std::printf("  kTLS 지원: %s\n",
                supported ? "yes (Linux 4.13+ CONFIG_TLS)" : "no (폴백 필요)");

    // TLS 세션 파라미터 구성 (AES-128-GCM 예시)
    ktls::TlsParams params;
    params.version    = ktls::TlsVersion::TLS_1_3;
    params.cipher     = ktls::TlsCipher::AES_128_GCM_SHA256;
    params.key        = {/* 16바이트 AES 키 */};
    params.iv         = {/* 12바이트 IV */};
    params.seq        = 0;

    std::printf("  TLS 버전: %s\n",
                params.version == ktls::TlsVersion::TLS_1_3 ? "1.3" : "1.2");
    std::printf("  암호화: AES-128-GCM-SHA256\n");

    // 실제 TLS 핸드셰이크 완료 소켓에 적용 (fd=-1이므로 실패 예상)
    int fake_fd = -1;
    auto tx_r = ktls::enable_tx(fake_fd, params);
    std::printf("  enable_tx(fake_fd): %s\n",
                tx_r ? "성공" : tx_r.error().message().c_str());

    // sendfile + kTLS 패턴 설명
    std::printf("  sendfile_tls() 패턴:\n");
    std::printf("    // 핸드셰이크 완료 후:\n");
    std::printf("    ktls::enable_tx(conn_fd, session_params);\n");
    std::printf("    ktls::enable_rx(conn_fd, session_params);\n");
    std::printf("    // 이후 send()/recv() → 커널이 직접 암호화\n");
    std::printf("    ktls::sendfile_tls(conn_fd, file_fd, 0, file_size);\n\n");
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

    std::printf("=== 완료 ===\n");
    return 0;
}
