#pragma once
/**
 * @file qbuem/core/thread.hpp
 * @brief `qbuem::Thread` — qbuem-stack's first-class thread component.
 *
 * A single owned threading primitive used everywhere in the stack (instead of
 * `std::jthread` / raw `std::thread`). Rationale:
 *
 *  - **Portable.** `std::jthread` is absent from Apple's libc++ (Xcode <= 15.x),
 *    so any direct use broke Clang+Apple-libc++ builds. `Thread` wraps the
 *    universally-available `std::thread` (POSIX pthread underneath) plus
 *    `std::stop_token` cooperative cancellation, so it works on every supported
 *    toolchain with one code path that is always the one we test.
 *  - **`std::jthread` semantics.** Move-only; the destructor and move-assignment
 *    `request_stop()` then `join()`. A stop-aware callable (first parameter
 *    `std::stop_token`) is handed the token automatically.
 *  - **Extensible.** Being our own component, it can carry server-grade extras
 *    such as thread naming (below) and — later — CPU pinning for core-sharding,
 *    which `std::jthread` could never expose.
 *
 * POSIX only (Linux / macOS), matching the rest of qbuem-stack.
 */
#include <stop_token>   // std::stop_token / std::stop_source — present on Apple libc++ (only jthread is not)
#include <thread>
#include <type_traits>
#include <utility>

#include <pthread.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif

namespace qbuem {

class Thread {
public:
    using id                 = std::thread::id;
    using native_handle_type = std::thread::native_handle_type;

    Thread() noexcept = default;

    template <class Fn, class... Args,
              std::enable_if_t<!std::is_same_v<std::remove_cvref_t<Fn>, Thread>, int> = 0>
    explicit Thread(Fn&& fn, Args&&... args) {
        if constexpr (std::is_invocable_v<std::decay_t<Fn>, std::stop_token, std::decay_t<Args>...>) {
            // Stop-aware callable: inject the token as the first argument.
            t_ = std::thread(std::forward<Fn>(fn), ss_.get_token(), std::forward<Args>(args)...);
        } else {
            static_assert(std::is_invocable_v<std::decay_t<Fn>, std::decay_t<Args>...>,
                          "qbuem::Thread: callable is not invocable with the given arguments");
            t_ = std::thread(std::forward<Fn>(fn), std::forward<Args>(args)...);
        }
    }

    Thread(const Thread&)            = delete;
    Thread& operator=(const Thread&) = delete;

    Thread(Thread&&) noexcept = default;   // target is freshly-constructed (no live thread to stop/join)

    Thread& operator=(Thread&& other) noexcept {
        if (this != &other) {
            if (t_.joinable()) { ss_.request_stop(); t_.join(); }
            ss_ = std::move(other.ss_);
            t_  = std::move(other.t_);
        }
        return *this;
    }

    ~Thread() {
        if (t_.joinable()) { ss_.request_stop(); t_.join(); }
    }

    [[nodiscard]] bool joinable() const noexcept { return t_.joinable(); }
    void join()   { t_.join(); }
    void detach() { t_.detach(); }

    bool request_stop() noexcept { return ss_.request_stop(); }
    [[nodiscard]] std::stop_source get_stop_source() noexcept { return ss_; }
    [[nodiscard]] std::stop_token  get_stop_token() const noexcept { return ss_.get_token(); }

    [[nodiscard]] id                 get_id() const noexcept { return t_.get_id(); }
    [[nodiscard]] native_handle_type native_handle() { return t_.native_handle(); }

    void swap(Thread& o) noexcept { std::swap(ss_, o.ss_); std::swap(t_, o.t_); }

    // ── Server extras (portable, best-effort) ───────────────────────────────
    /**
     * @brief Name the CALLING thread (visible in top -H, gdb, Instruments).
     *
     * Both platforms can only reliably name the running thread, so call this
     * from inside the thread body. Linux truncates to 15 chars. Never throws,
     * never fails the build (uses prctl on Linux — no _GNU_SOURCE needed).
     */
    static void set_current_name(const char* name) noexcept {
#if defined(__APPLE__)
        ::pthread_setname_np(name);
#elif defined(__linux__)
        ::prctl(PR_SET_NAME, reinterpret_cast<unsigned long>(name), 0UL, 0UL, 0UL);
#else
        (void)name;
#endif
    }

    // NOTE: CPU pinning (pin_current_to_cpu) is deferred to the core-sharding
    // work (P1) — it needs _GNU_SOURCE/CPU_SET on Linux and is a no-op on macOS,
    // so it is added when the sharded reactor lands and can be tested for real.

private:
    std::stop_source ss_;   // declared before t_: outlives the thread on teardown
    std::thread      t_;
};

inline void swap(Thread& a, Thread& b) noexcept { a.swap(b); }

}  // namespace qbuem
