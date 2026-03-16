/**
 * @file zero_copy_arena_channel_example.cpp
 * @brief Zero-copy I/O + ArenaChannel + AsyncLogger 예제.
 *
 * ## 커버리지 — io/zero_copy.hpp
 * - zero_copy::sendfile()      — sendfile(2) 기반 파일→소켓 제로 카피
 * - zero_copy::splice()        — splice(2) 기반 파이프 기반 제로 카피
 *
 * ## 커버리지 — pipeline/arena_channel.hpp
 * - ArenaChannel<T>            — reactor-local zero-alloc 채널
 * - ArenaChannel::push()       — O(1) 논블로킹 송신 (FixedPoolResource 기반)
 * - ArenaChannel::pop()        — O(1) 논블로킹 수신
 * - ArenaChannel::size()       — 현재 채널 크기
 * - ArenaChannel::capacity()   — 채널 용량
 *
 * ## 커버리지 — core/async_logger.hpp
 * - AsyncLogger(capacity)      — 링 버퍼 비동기 로거 생성
 * - AsyncLogger::start()       — 백그라운드 플러시 스레드 시작
 * - AsyncLogger::log()         — 핫패스 로그 엔큐 (O(1), 논블로킹)
 * - AsyncLogger::stop()        — 남은 항목 플러시 후 종료
 * - AsyncLogger::make_callback()— App 액세스 로거 콜백 생성
 */

#include <qbuem/core/async_logger.hpp>
#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/io/zero_copy.hpp>
#include <qbuem/pipeline/arena_channel.hpp>

#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace qbuem;
using namespace std::chrono_literals;

// ─────────────────────────────────────────────────────────────────────────────
// §1  ArenaChannel — reactor-local zero-alloc 채널
// ─────────────────────────────────────────────────────────────────────────────

static void demo_arena_channel() {
    std::printf("── §1  ArenaChannel ──\n");

    // 128 슬롯 ArenaChannel 생성 (힙 1회만 할당)
    ArenaChannel<int> chan(128);
    std::printf("  용량: %zu\n", chan.capacity());

    // 생산자: push
    for (int i = 0; i < 10; ++i) {
        bool ok = chan.push(i * 100);
        if (!ok) { std::printf("  push 실패 (채널 꽉 참)\n"); break; }
    }
    std::printf("  push 10개 후 크기: %zu\n", chan.size());

    // 소비자: pop
    std::vector<int> results;
    while (auto v = chan.pop())
        results.push_back(*v);
    std::printf("  pop 결과 (%zu개):", results.size());
    for (int v : results) std::printf(" %d", v);
    std::printf("\n");

    // 용량 테스트: 꽉 찼을 때 push는 false
    ArenaChannel<std::string> small_chan(4);
    for (int i = 0; i < 6; ++i) {
        bool ok = small_chan.push("item" + std::to_string(i));
        std::printf("  small_chan push(%d): %s\n", i, ok ? "ok" : "full");
    }
    std::printf("\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// §2  Zero-copy I/O — sendfile / splice
// ─────────────────────────────────────────────────────────────────────────────

static std::atomic<bool> g_zero_copy_done{false};

static Task<void> demo_zero_copy_task() {
    std::printf("── §2  Zero-copy I/O ──\n");

    // 임시 파일 생성
    char tmpfile[] = "/tmp/qbuem_zero_copy_XXXXXX";
    int in_fd = ::mkstemp(tmpfile);
    if (in_fd < 0) {
        std::printf("  임시 파일 생성 실패\n");
        g_zero_copy_done.store(true);
        co_return;
    }

    // 파일에 데이터 쓰기
    const char* content = "Hello, zero-copy sendfile world!\n"
                          "This data is transferred without user-space copy.\n";
    ssize_t written = ::write(in_fd, content, strlen(content));
    ::lseek(in_fd, 0, SEEK_SET);  // 처음으로 되감기

    std::printf("  임시 파일 생성: %s (%zd 바이트)\n", tmpfile, written);

    // 소켓 쌍 생성 (sendfile 테스트용)
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        std::printf("  socketpair 실패\n");
        ::close(in_fd);
        ::unlink(tmpfile);
        g_zero_copy_done.store(true);
        co_return;
    }

    // sendfile: 파일 → 소켓 (zero-copy)
    auto sent_r = co_await zero_copy::sendfile(
        sv[1],       // 출력: 소켓
        in_fd,       // 입력: 파일
        0,           // 오프셋
        written      // 전송할 바이트 수
    );

    if (sent_r) {
        std::printf("  sendfile(): %zu 바이트 전송 완료\n", *sent_r);

        // 수신 측에서 데이터 읽기
        std::array<char, 256> buf{};
        ssize_t n = ::recv(sv[0], buf.data(), buf.size(), MSG_DONTWAIT);
        if (n > 0) {
            std::printf("  수신: %zd 바이트 (내용 일치: %s)\n",
                        n, (memcmp(buf.data(), content, n) == 0) ? "yes" : "no");
        }
    } else {
        std::printf("  sendfile(): %s\n",
                    sent_r.error().message().c_str());
    }

#ifdef __linux__
    // splice: 파이프 기반 zero-copy (Linux 전용)
    int pipefd[2];
    if (::pipe(pipefd) == 0) {
        ::lseek(in_fd, 0, SEEK_SET);

        auto splice_r = co_await zero_copy::splice(
            in_fd,      // 입력 fd
            pipefd[1],  // 파이프 쓰기 端
            written     // 전송할 바이트
        );

        if (splice_r) {
            std::printf("  splice(file→pipe): %zu 바이트\n", *splice_r);

            // 파이프에서 소켓으로 splice
            auto splice2_r = co_await zero_copy::splice(
                pipefd[0],  // 파이프 읽기 端
                sv[1],      // 소켓
                *splice_r
            );
            if (splice2_r)
                std::printf("  splice(pipe→socket): %zu 바이트\n", *splice2_r);
        } else {
            std::printf("  splice(): %s\n", splice_r.error().message().c_str());
        }

        ::close(pipefd[0]);
        ::close(pipefd[1]);
    }
#else
    std::printf("  splice(): Linux 전용 (이 플랫폼에서는 미지원)\n");
#endif

    ::close(sv[0]);
    ::close(sv[1]);
    ::close(in_fd);
    ::unlink(tmpfile);
    std::printf("\n");

    g_zero_copy_done.store(true);
    co_return;
}

// ─────────────────────────────────────────────────────────────────────────────
// §3  AsyncLogger — 비동기 액세스 로거
// ─────────────────────────────────────────────────────────────────────────────

static void demo_async_logger() {
    std::printf("── §3  AsyncLogger ──\n");

    // 4096 항목 링 버퍼 로거
    AsyncLogger logger(4096);
    logger.start();  // 백그라운드 플러시 스레드 시작

    std::printf("  로그 100건 엔큐 중...\n");

    // 핫패스에서 O(1) 논블로킹 로그 기록
    for (int i = 0; i < 100; ++i) {
        logger.log(
            "GET",                           // HTTP 메서드
            "/api/users/" + std::to_string(i), // 경로
            200,                             // 상태 코드
            (i % 10) * 100 + 500             // 응답 시간 (µs)
        );
    }

    // 에러 로그
    logger.log("POST", "/api/orders", 500, 25000);
    logger.log("DELETE", "/api/users/42", 403, 800);

    // App 액세스 로거 콜백 생성
    auto cb = logger.make_callback();
    cb("GET", "/health", 200, 50);  // 콜백을 통한 로그 기록

    std::printf("  로그 103건 기록 완료\n");

    logger.stop();  // 남은 항목 플러시 후 스레드 종료
    std::printf("  AsyncLogger 종료 완료\n\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::printf("=== qbuem Zero-copy + ArenaChannel + AsyncLogger 예제 ===\n\n");

    demo_arena_channel();
    demo_async_logger();

    // Zero-copy는 코루틴 기반
    Dispatcher disp(1);
    std::thread t([&] { disp.run(); });

    disp.spawn([&]() -> Task<void> { co_await demo_zero_copy_task(); }());

    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!g_zero_copy_done.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);

    disp.stop();
    t.join();

    std::printf("=== 완료 ===\n");
    return 0;
}
