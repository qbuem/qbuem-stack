/**
 * @file examples/arena_example.cpp
 * @brief Arena 할당자 + FixedPoolResource + AsyncLogger 예시
 */
#include <qbuem/core/arena.hpp>
#include <qbuem/core/async_logger.hpp>

#include <cstring>
#include <iostream>
#include <string>

using namespace qbuem;

// ─── Arena 예시 ─────────────────────────────────────────────────────────────

struct HttpHeader {
    char name[64];
    char value[128];
};

void arena_example() {
    std::cout << "=== Arena (bump-pointer) ===\n";

    Arena arena(4096);

    // O(1) 할당 — HTTP 요청 처리 시 헤더를 Arena에 할당
    auto* h1 = static_cast<HttpHeader*>(
        arena.allocate(sizeof(HttpHeader), alignof(HttpHeader)));
    std::strncpy(h1->name,  "Content-Type",  sizeof(h1->name) - 1);
    std::strncpy(h1->value, "application/json", sizeof(h1->value) - 1);

    auto* h2 = static_cast<HttpHeader*>(
        arena.allocate(sizeof(HttpHeader), alignof(HttpHeader)));
    std::strncpy(h2->name,  "Authorization", sizeof(h2->name) - 1);
    std::strncpy(h2->value, "Bearer tok123",  sizeof(h2->value) - 1);

    std::cout << "[arena] h1: " << h1->name << ": " << h1->value << "\n";
    std::cout << "[arena] h2: " << h2->name << ": " << h2->value << "\n";

    // 다양한 크기 할당
    auto* buf = static_cast<uint8_t*>(arena.allocate(1024));
    std::memset(buf, 0xAB, 1024);
    std::cout << "[arena] 1024-byte buffer allocated, buf[0]=0x"
              << std::hex << (int)buf[0] << std::dec << "\n";

    // reset() — O(1), 포인터만 초기화 (메모리 반환 없음)
    arena.reset();
    std::cout << "[arena] reset() done — pointers cleared\n";

    // reset 후 재사용
    auto* recycled = static_cast<HttpHeader*>(
        arena.allocate(sizeof(HttpHeader), alignof(HttpHeader)));
    std::strncpy(recycled->name, "X-Request-ID", sizeof(recycled->name) - 1);
    std::cout << "[arena] Recycled: " << recycled->name << "\n";
}

// ─── FixedPoolResource 예시 ────────────────────────────────────────────────

struct Connection {
    int fd;
    char peer_addr[64];
    size_t bytes_read;
};

void pool_example() {
    std::cout << "\n=== FixedPoolResource (free-list pool) ===\n";

    // 최대 32개 Connection 슬롯 풀
    FixedPoolResource<sizeof(Connection), alignof(Connection)> pool(32);

    // O(1) 할당
    auto* c1 = static_cast<Connection*>(pool.allocate());
    c1->fd = 10;
    std::strncpy(c1->peer_addr, "192.168.1.1:54321", sizeof(c1->peer_addr) - 1);
    c1->bytes_read = 0;

    auto* c2 = static_cast<Connection*>(pool.allocate());
    c2->fd = 11;
    std::strncpy(c2->peer_addr, "10.0.0.5:12345", sizeof(c2->peer_addr) - 1);
    c2->bytes_read = 0;

    std::cout << "[pool] c1: fd=" << c1->fd << " peer=" << c1->peer_addr << "\n";
    std::cout << "[pool] c2: fd=" << c2->fd << " peer=" << c2->peer_addr << "\n";

    // O(1) 해제 — free-list 헤드에 추가
    pool.deallocate(c1);
    std::cout << "[pool] c1 released\n";

    // 해제된 슬롯 재사용
    auto* c3 = static_cast<Connection*>(pool.allocate());
    c3->fd = 12;
    std::strncpy(c3->peer_addr, "172.16.0.1:9999", sizeof(c3->peer_addr) - 1);
    std::cout << "[pool] c3 (reused slot): fd=" << c3->fd << " peer=" << c3->peer_addr << "\n";

    // 풀 용량 초과 시 nullptr 반환
    std::vector<void*> slots;
    while (auto* s = pool.allocate()) slots.push_back(s);
    auto* overflow = pool.allocate();
    std::cout << "[pool] Overflow allocate: "
              << (overflow == nullptr ? "nullptr (expected)" : "non-null (ERROR)") << "\n";

    for (auto* s : slots) pool.deallocate(s);
    pool.deallocate(c2);
    pool.deallocate(c3);
}

// ─── AsyncLogger 예시 ──────────────────────────────────────────────────────

void async_logger_example() {
    std::cout << "\n=== AsyncLogger (lock-free SPSC ring) ===\n";

    AsyncLogger logger(1024);  // 1024-entry ring buffer
    logger.start();

    // 핫패스에서 논블로킹 로그 기록
    logger.log("GET",    "/api/users",    200, 1234);
    logger.log("POST",   "/api/orders",   201, 5678);
    logger.log("DELETE", "/api/items/42", 204,  123);
    logger.log("GET",    "/healthz",      200,   10);

    // App::set_access_logger() 통합
    auto cb = logger.make_callback();
    cb("PUT", "/api/config", 400, 999);  // 직접 콜백 호출 테스트

    std::cout << "[logger] 5 log entries queued (async flush)\n";

    logger.stop();  // 잔여 항목 플러시 후 join
    std::cout << "[logger] Flushed and stopped\n";
}

int main() {
    arena_example();
    pool_example();
    async_logger_example();
    return 0;
}
