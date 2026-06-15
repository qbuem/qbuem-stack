#pragma once

/**
 * @file qbuem/buf/inplace_function.hpp
 * @brief Small-buffer (zero-allocation) type-erased callable.
 * @ingroup qbuem_buf
 *
 * `inplace_function<Sig, N>` is a drop-in alternative to `std::function` for
 * hot paths. Unlike `std::function`, it **never heap-allocates**: the target
 * callable is stored in an inline `N`-byte buffer (a compile-time error if it
 * does not fit). This directly serves the Zero-Allocation pillar — `std::function`
 * allocates on the heap for any closure larger than its (~16-byte) small-buffer
 * optimization, which on a per-I/O or per-message path is a hidden malloc.
 *
 * It is **move-only** (the hot-path callback idiom is construct → store → move,
 * never copy) and the target must be nothrow-move-constructible.
 *
 * ```cpp
 * qbuem::inplace_function<void(int)> cb = [handle, this](int fd) { resume(fd); };
 * cb(fd);                       // indirect call, no allocation, ever
 * static_assert(sizeof(cb) == 48 + 3*sizeof(void*));  // inline buffer + vtable ptrs
 * ```
 *
 * Intended replacements: `Reactor::register_event` callbacks, pipeline `Action`
 * stage storage, timer callbacks — anywhere `std::function` sits on a hot path.
 */

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace qbuem {

template <typename Signature, std::size_t Capacity = 48,
          std::size_t Align = alignof(std::max_align_t)>
class inplace_function;

template <typename R, typename... Args, std::size_t Capacity, std::size_t Align>
class inplace_function<R(Args...), Capacity, Align> {
public:
    inplace_function() noexcept = default;
    inplace_function(std::nullptr_t) noexcept {}

    /// Construct from any compatible callable that fits the inline buffer.
    template <typename F, typename DF = std::decay_t<F>,
              typename = std::enable_if_t<
                  !std::is_same_v<DF, inplace_function> &&
                  std::is_invocable_r_v<R, DF&, Args...>>>
    inplace_function(F&& f) noexcept {
        static_assert(sizeof(DF) <= Capacity,
                      "callable too large for inplace_function capacity (raise N)");
        static_assert(alignof(DF) <= Align,
                      "callable over-aligned for inplace_function");
        static_assert(std::is_nothrow_move_constructible_v<DF>,
                      "inplace_function target must be nothrow-move-constructible");
        ::new (storage()) DF(std::forward<F>(f));
        invoke_  = [](void* p, Args... a) -> R {
            return (*static_cast<DF*>(p))(std::forward<Args>(a)...);
        };
        move_    = [](void* dst, void* src) noexcept {
            ::new (dst) DF(std::move(*static_cast<DF*>(src)));
            static_cast<DF*>(src)->~DF();
        };
        destroy_ = [](void* p) noexcept { static_cast<DF*>(p)->~DF(); };
    }

    inplace_function(inplace_function&& o) noexcept { move_from(o); }
    inplace_function& operator=(inplace_function&& o) noexcept {
        if (this != &o) { reset(); move_from(o); }
        return *this;
    }
    inplace_function(const inplace_function&)            = delete;
    inplace_function& operator=(const inplace_function&) = delete;

    ~inplace_function() { reset(); }

    [[nodiscard]] explicit operator bool() const noexcept { return invoke_ != nullptr; }

    R operator()(Args... a) const {
        return invoke_(storage(), std::forward<Args>(a)...);
    }

    void reset() noexcept {
        if (destroy_) destroy_(storage());
        invoke_ = nullptr; move_ = nullptr; destroy_ = nullptr;
    }

private:
    void* storage() const noexcept { return const_cast<std::byte*>(buf_); }

    void move_from(inplace_function& o) noexcept {
        invoke_ = o.invoke_; move_ = o.move_; destroy_ = o.destroy_;
        if (move_) move_(storage(), o.storage());
        o.invoke_ = nullptr; o.move_ = nullptr; o.destroy_ = nullptr;
    }

    alignas(Align) std::byte buf_[Capacity];
    R (*invoke_)(void*, Args...)         = nullptr;
    void (*move_)(void*, void*) noexcept = nullptr;
    void (*destroy_)(void*) noexcept     = nullptr;
};

} // namespace qbuem
