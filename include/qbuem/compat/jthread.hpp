#pragma once
/**
 * @file qbuem/compat/jthread.hpp
 * @brief Portable `qbuem::jthread` — a drop-in for `std::jthread`.
 *
 * Apple's libc++ (Xcode <= 15.x, the toolchain on current macOS CI runners and
 * many user machines) ships `<stop_token>` (std::stop_token / std::stop_source)
 * but does NOT declare `std::jthread`. That makes every qbuem-stack consumer
 * built with Apple clang fail to compile. `qbuem::jthread` closes that gap:
 *
 *   - Where the standard library provides `std::jthread`, this is a transparent
 *     `using` alias (zero behaviour change).
 *   - Otherwise it is a small, conforming-enough implementation built on
 *     `std::thread` + `std::stop_source` (which Apple's libc++ does provide):
 *     auto request_stop()+join() on destruction, a `std::stop_token` first
 *     argument injected into stop-aware callables, `request_stop()`,
 *     `get_stop_source()/get_stop_token()`, move-only semantics.
 *
 * Define `QBUEM_FORCE_PORTABLE_JTHREAD` to exercise the portable path even on
 * platforms that have `std::jthread` (used by the test suite to validate it).
 */
#include <version>

#if defined(__cpp_lib_jthread) && (__cpp_lib_jthread >= 201911L) && !defined(QBUEM_FORCE_PORTABLE_JTHREAD)

#include <stop_token>
#include <thread>
namespace qbuem {
using std::jthread;
}  // namespace qbuem

#else  // ── portable implementation ─────────────────────────────────────────

#include <stop_token>   // std::stop_token / std::stop_source (present on Apple libc++)
#include <thread>
#include <type_traits>
#include <utility>

namespace qbuem {

/// Drop-in subset of std::jthread (move-only; auto request_stop()+join() on dtor).
class jthread {
public:
    using id          = std::thread::id;
    using native_handle_type = std::thread::native_handle_type;

    jthread() noexcept = default;

    template <class Fn, class... Args,
              std::enable_if_t<!std::is_same_v<std::remove_cvref_t<Fn>, jthread>, int> = 0>
    explicit jthread(Fn&& fn, Args&&... args) {
        if constexpr (std::is_invocable_v<std::decay_t<Fn>, std::stop_token, std::decay_t<Args>...>) {
            // Stop-aware callable: inject the token as the first argument (std::jthread semantics).
            t_ = std::thread(std::forward<Fn>(fn), ss_.get_token(), std::forward<Args>(args)...);
        } else {
            static_assert(std::is_invocable_v<std::decay_t<Fn>, std::decay_t<Args>...>,
                          "qbuem::jthread: callable is not invocable with the given arguments");
            t_ = std::thread(std::forward<Fn>(fn), std::forward<Args>(args)...);
        }
    }

    jthread(const jthread&)            = delete;
    jthread& operator=(const jthread&) = delete;

    jthread(jthread&&) noexcept = default;   // target is freshly-constructed (no live thread to join)

    jthread& operator=(jthread&& other) noexcept {
        if (this != &other) {
            if (t_.joinable()) { ss_.request_stop(); t_.join(); }  // stop+join the one we hold
            ss_ = std::move(other.ss_);
            t_  = std::move(other.t_);
        }
        return *this;
    }

    ~jthread() {
        if (t_.joinable()) { ss_.request_stop(); t_.join(); }
    }

    [[nodiscard]] bool joinable() const noexcept { return t_.joinable(); }
    void join()   { t_.join(); }
    void detach() { t_.detach(); }

    bool request_stop() noexcept { return ss_.request_stop(); }
    [[nodiscard]] std::stop_source get_stop_source() noexcept { return ss_; }
    [[nodiscard]] std::stop_token  get_stop_token() const noexcept { return ss_.get_token(); }

    [[nodiscard]] id get_id() const noexcept { return t_.get_id(); }
    [[nodiscard]] native_handle_type native_handle() { return t_.native_handle(); }

    void swap(jthread& o) noexcept { std::swap(ss_, o.ss_); std::swap(t_, o.t_); }

private:
    std::stop_source ss_;   // declared before t_: outlives the thread on teardown
    std::thread      t_;
};

inline void swap(jthread& a, jthread& b) noexcept { a.swap(b); }

}  // namespace qbuem

#endif
