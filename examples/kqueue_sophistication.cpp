#include <qbuem/core/kqueue_reactor.hpp>
#include <qbuem/buf/kqueue_buffer_pool.hpp>
#include <iostream>
#include <chrono>
#include <unistd.h>
#include <fcntl.h>
#include <string>
#include <vector>
#include <array>

using namespace qbuem;

int main() {
    std::cout << "--- qbuem-stack kqueue Sophistication Demo ---" << std::endl;

    qbuem::KqueueReactor reactor;
    qbuem::KqueueBufferPool pool(4096, 64);

    // 1. Demonstrate Multi-event Batching & Pointer-direct Dispatch
    std::array<int, 2> pipe_fds;
    if (pipe(pipe_fds.data()) == -1) return 1;
    fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK);

    auto start_time = std::chrono::steady_clock::now();

    reactor.register_event(pipe_fds[0], qbuem::EventType::Read, [&](int fd) {
        auto end_time = std::chrono::steady_clock::now();
        auto diff = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
        std::cout << "[Event] Received data on fd " << fd << " in " << diff.count() << "us" << std::endl;
        
        // Use User-space Buffer Ring
        auto buf = pool.acquire();
        if (buf.addr) {
            ssize_t n = read(fd, buf.addr, buf.len);
            if (n > 0) {
                std::cout << "[Buffer] Read " << n << " bytes into buffer ID " << buf.bid << std::endl;
                std::cout << "[Buffer] Content: " << std::string(static_cast<char*>(buf.addr), static_cast<size_t>(n)) << std::endl;
            }
            pool.release(buf.bid);
        }
        
        reactor.stop();
    });

    // Write some data to trigger the event
    const char* msg = "Hello sophisticated kqueue!";
    write(pipe_fds[1], msg, 27);

    std::cout << "[Main] Polling..." << std::endl;
    while (reactor.is_running()) {
        reactor.poll(100);
    }

    close(pipe_fds[0]);
    close(pipe_fds[1]);

    // 2. Demonstrate Batching with multiple events
    std::cout << "\n[Main] Demonstrating batching with timers..." << std::endl;
    for (int i = 0; i < 10; ++i) {
        reactor.register_timer(10 * (i + 1), [i](int id) {
            std::cout << "[Timer] Timer " << i << " (id " << id << ") fired" << std::endl;
        });
    }

    int timer_count = 0;
    while (timer_count < 10) {
        auto polled = reactor.poll(100);
        if (polled) {
            timer_count += polled.value();
        }
    }

    std::cout << "--- Demo completed ---" << std::endl;

    return 0;
}
