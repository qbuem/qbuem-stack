/**
 * @file examples/arena_example.cpp
 * @brief Arena allocator + FixedPoolResource + AsyncLogger example.
 */
#include <qbuem/core/arena.hpp>
#include <qbuem/core/async_logger.hpp>

#include <cstring>
#include <string>
#include <qbuem/compat/print.hpp>

using namespace qbuem;
using std::println;
using std::print;

// ─── Arena example ───────────────────────────────────────────────────────────

struct HttpHeader {
    char name[64];
    char value[128];
};

void arena_example() {
    println("=== Arena (bump-pointer) ===");

    Arena arena(4096);

    // O(1) allocation — allocate headers in the Arena per HTTP request
    auto* h1 = static_cast<HttpHeader*>(
        arena.allocate(sizeof(HttpHeader), alignof(HttpHeader)));
    std::strncpy(h1->name,  "Content-Type",  sizeof(h1->name) - 1);
    std::strncpy(h1->value, "application/json", sizeof(h1->value) - 1);

    auto* h2 = static_cast<HttpHeader*>(
        arena.allocate(sizeof(HttpHeader), alignof(HttpHeader)));
    std::strncpy(h2->name,  "Authorization", sizeof(h2->name) - 1);
    std::strncpy(h2->value, "Bearer tok123",  sizeof(h2->value) - 1);

    println("[arena] h1: {}: {}", h1->name, h1->value);
    println("[arena] h2: {}: {}", h2->name, h2->value);

    // Allocate various sizes
    auto* buf = static_cast<uint8_t*>(arena.allocate(1024));
    std::memset(buf, 0xAB, 1024);
    println("[arena] 1024-byte buffer allocated, buf[0]=0x{:x}", buf[0]);

    // reset() — O(1), resets pointer only (no memory deallocation)
    arena.reset();
    println("[arena] reset() done — pointers cleared");

    // Reuse after reset
    auto* recycled = static_cast<HttpHeader*>(
        arena.allocate(sizeof(HttpHeader), alignof(HttpHeader)));
    std::strncpy(recycled->name, "X-Request-ID", sizeof(recycled->name) - 1);
    println("[arena] Recycled: {}", recycled->name);
}

// ─── FixedPoolResource example ────────────────────────────────────────────────

struct Connection {
    int fd;
    char peer_addr[64];
    size_t bytes_read;
};

void pool_example() {
    println("\n=== FixedPoolResource (free-list pool) ===");

    // Pool of up to 32 Connection slots
    FixedPoolResource<sizeof(Connection), alignof(Connection)> pool(32);

    // O(1) allocation
    auto* c1 = static_cast<Connection*>(pool.allocate());
    c1->fd = 10;
    std::strncpy(c1->peer_addr, "192.168.1.1:54321", sizeof(c1->peer_addr) - 1);
    c1->bytes_read = 0;

    auto* c2 = static_cast<Connection*>(pool.allocate());
    c2->fd = 11;
    std::strncpy(c2->peer_addr, "10.0.0.5:12345", sizeof(c2->peer_addr) - 1);
    c2->bytes_read = 0;

    println("[pool] c1: fd={} peer={}", c1->fd, c1->peer_addr);
    println("[pool] c2: fd={} peer={}", c2->fd, c2->peer_addr);

    // O(1) deallocation — prepend to free-list head
    pool.deallocate(c1);
    println("[pool] c1 released");

    // Reuse the freed slot
    auto* c3 = static_cast<Connection*>(pool.allocate());
    c3->fd = 12;
    std::strncpy(c3->peer_addr, "172.16.0.1:9999", sizeof(c3->peer_addr) - 1);
    println("[pool] c3 (reused slot): fd={} peer={}", c3->fd, c3->peer_addr);

    // Overflow returns nullptr when pool is exhausted
    std::vector<void*> slots;
    while (auto* s = pool.allocate()) slots.push_back(s);
    auto* overflow = pool.allocate();
    println("[pool] Overflow allocate: {}",
              overflow == nullptr ? "nullptr (expected)" : "non-null (ERROR)");

    for (auto* s : slots) pool.deallocate(s);
    pool.deallocate(c2);
    pool.deallocate(c3);
}

// ─── AsyncLogger example ──────────────────────────────────────────────────────

void async_logger_example() {
    println("\n=== AsyncLogger (lock-free SPSC ring) ===");

    AsyncLogger logger(1024);  // 1024-entry ring buffer
    logger.start();

    // Non-blocking log writes on the hot path
    logger.log("GET",    "/api/users",    200, 1234);
    logger.log("POST",   "/api/orders",   201, 5678);
    logger.log("DELETE", "/api/items/42", 204,  123);
    logger.log("GET",    "/healthz",      200,   10);

    // App::set_access_logger() integration
    auto cb = logger.make_callback();
    cb("PUT", "/api/config", 400, 999);  // direct callback invocation test

    println("[logger] 5 log entries queued (async flush)");

    logger.stop();  // flush remaining entries then join
    println("[logger] Flushed and stopped");
}

int main() {
    arena_example();
    pool_example();
    async_logger_example();
    return 0;
}
