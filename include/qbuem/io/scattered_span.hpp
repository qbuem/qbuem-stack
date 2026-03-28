#pragma once

/**
 * @file qbuem/io/scattered_span.hpp
 * @brief Non-owning scatter-gather view over discontiguous byte buffers.
 * @defgroup qbuem_scattered_span ScatteredSpan
 * @ingroup qbuem_io_buffers
 *
 * `scattered_span` is a lightweight, zero-allocation, non-owning view over an
 * array of `iovec` entries representing discontiguous read-only byte buffers.
 *
 * ## Design
 *
 * The type is a pure **view** — it stores a single `std::span<const iovec>`
 * pointing into caller-owned storage (e.g. a stack-allocated `IOVec<N>`).
 * There is no heap allocation, no copy, and no ownership.
 *
 * - **Zero allocation**: only a `std::span<const iovec>` is stored.
 * - **Zero copy**: segments are not gathered; the iovec array is passed
 *   directly to `readv(2)` / `writev(2)` / `sendmsg(2)` / `recvmsg(2)`.
 * - **Range compatible**: iteration yields `std::span<const std::byte>` segments.
 * - **Syscall ready**: `iov_data()` / `iov_count()` / `as_iovec()` map
 *   directly to POSIX scatter-gather arguments.
 *
 * ## Lifetime contract
 *
 * The backing iovec array **must outlive** the `scattered_span`. The typical
 * idiom is to keep the `IOVec<N>` on the stack in the same scope:
 *
 * @code
 * IOVec<2> vec;
 * vec.push(header.data(), header.size());
 * vec.push(body.data(),   body.size());
 * scattered_span scatter{vec};
 * // scatter is valid as long as vec is in scope.
 * @endcode
 *
 * ## Integration with existing APIs
 *
 * | API | How to use scattered_span |
 * |-----|--------------------------|
 * | `TcpStream::writev(span<const iovec>)` | Pass `scatter.as_iovec()` or let implicit conversion apply |
 * | `TcpStream::readv(span<iovec>)` | Build `IOVec<N>` of mutable bufs, pass `vec.as_span()` directly |
 * | `uds::send_fds(sockfd, fds, data)` | Use `scattered_span` overload for multi-segment payloads |
 * | `Http1Handler` response send | `IOVec<2>{header, body}` → `scattered_span` → single `writev` |
 * | `::writev(fd, ...)` raw syscall | `::writev(fd, scatter.iov_data(), scatter.iov_count())` |
 * | `::sendmsg` | Set `msg.msg_iov = scatter.iov_data(); msg.msg_iovlen = scatter.iov_count()` |
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/io/iovec.hpp>

#include <cassert>
#include <cstddef>
#include <iterator>
#include <span>
#include <sys/uio.h>

namespace qbuem {

// ─── scattered_span ───────────────────────────────────────────────────────────

/**
 * @brief Non-owning scatter-gather view over discontiguous const byte buffers.
 *
 * Wraps a `std::span<const iovec>` and provides:
 * - A C++ random-access range whose elements are `std::span<const std::byte>` segments.
 * - Direct POSIX syscall accessors (`iov_data()`, `iov_count()`, `as_iovec()`).
 * - Implicit conversion to `std::span<const iovec>` for API compatibility.
 *
 * The backing iovec storage is NOT owned. Lifetime is bounded by the underlying array.
 *
 * ### Thread safety
 * `scattered_span` is immutable after construction; reads are safe to share concurrently.
 */
class scattered_span {
public:
    // ─── Proxy iterator ───────────────────────────────────────────────────────

    /**
     * @brief Random-access iterator yielding `std::span<const std::byte>` segments.
     *
     * This is a proxy iterator — `operator*` returns by value (no dangling reference).
     */
    struct iterator {
        using iterator_category = std::random_access_iterator_tag;
        using value_type        = std::span<const std::byte>;
        using difference_type   = std::ptrdiff_t;
        using pointer           = void; // proxy iterator has no addressable element
        using reference         = value_type;

        [[nodiscard]] value_type operator*() const noexcept {
            return {static_cast<const std::byte*>(ptr_->iov_base), ptr_->iov_len};
        }

        [[nodiscard]] value_type operator[](difference_type n) const noexcept {
            return {static_cast<const std::byte*>(ptr_[n].iov_base), ptr_[n].iov_len};
        }

        iterator& operator++()    noexcept { ++ptr_; return *this; }
        iterator  operator++(int) noexcept { auto t = *this; ++ptr_; return t; }
        iterator& operator--()    noexcept { --ptr_; return *this; }
        iterator  operator--(int) noexcept { auto t = *this; --ptr_; return t; }

        iterator& operator+=(difference_type n) noexcept { ptr_ += n; return *this; }
        iterator& operator-=(difference_type n) noexcept { ptr_ -= n; return *this; }

        [[nodiscard]] friend iterator operator+(iterator it, difference_type n) noexcept { it += n; return it; }
        [[nodiscard]] friend iterator operator+(difference_type n, iterator it) noexcept { it += n; return it; }
        [[nodiscard]] friend iterator operator-(iterator it, difference_type n) noexcept { it -= n; return it; }
        [[nodiscard]] friend difference_type operator-(iterator a, iterator b) noexcept { return a.ptr_ - b.ptr_; }

        [[nodiscard]] bool operator==(const iterator&)  const noexcept = default;
        [[nodiscard]] auto operator<=>(const iterator&) const noexcept = default;

        const iovec* ptr_{nullptr};
    };

    using value_type      = std::span<const std::byte>;
    using size_type       = std::size_t;
    using difference_type = std::ptrdiff_t;
    using const_iterator  = iterator;

    // ─── Construction ──────────────────────────────────────────────────────────

    /** @brief Default-constructed empty scatter view (size = 0). */
    constexpr scattered_span() noexcept = default;

    /**
     * @brief Construct from an existing `std::span<const iovec>`.
     *
     * @param iovs  View over iovec array. The array must outlive this object.
     */
    constexpr explicit scattered_span(std::span<const iovec> iovs) noexcept
        : iovs_{iovs} {}

    /**
     * @brief Construct from a `std::span<iovec>` (mutable → const promotion).
     *
     * @param iovs  View over mutable iovec array. The array must outlive this object.
     */
    constexpr explicit scattered_span(std::span<iovec> iovs) noexcept
        : iovs_{iovs} {}

    /**
     * @brief Construct from a stack-allocated `IOVec<N>`.
     *
     * The `IOVec<N>` must outlive this `scattered_span`.
     *
     * @tparam N Compile-time capacity of the `IOVec`.
     */
    template<std::size_t N>
    constexpr explicit scattered_span(const IOVec<N>& v) noexcept
        : iovs_{v.as_const_span()} {}

    // ─── POSIX syscall interface ───────────────────────────────────────────────

    /**
     * @brief Pointer to the first `iovec` entry.
     *
     * Use as the `iov` argument to `readv(2)` / `writev(2)` / `sendmsg(2)`.
     *
     * @code
     * ::writev(fd, scatter.iov_data(), scatter.iov_count());
     * @endcode
     */
    [[nodiscard]] constexpr const iovec* iov_data() const noexcept { return iovs_.data(); }

    /**
     * @brief Number of `iovec` entries, typed as `int` for POSIX compatibility.
     *
     * Use as the `iovcnt` argument to `readv(2)` / `writev(2)`.
     */
    [[nodiscard]] constexpr int iov_count() const noexcept {
        return static_cast<int>(iovs_.size());
    }

    /**
     * @brief `std::span` view over the internal `iovec` array.
     *
     * Compatible with `TcpStream::writev(std::span<const iovec>)`.
     */
    [[nodiscard]] constexpr std::span<const iovec> as_iovec() const noexcept { return iovs_; }

    /**
     * @brief Implicit conversion to `std::span<const iovec>`.
     *
     * Allows a `scattered_span` to be passed directly to any API accepting
     * `std::span<const iovec>` without an explicit `.as_iovec()` call.
     *
     * @code
     * co_await stream.writev(scatter);  // implicit conversion
     * @endcode
     */
    [[nodiscard]] constexpr operator std::span<const iovec>() const noexcept { return iovs_; }

    // ─── Size ──────────────────────────────────────────────────────────────────

    /** @brief Number of segments in this scatter view. */
    [[nodiscard]] constexpr std::size_t size() const noexcept { return iovs_.size(); }

    /** @brief Returns `true` if there are no segments. */
    [[nodiscard]] constexpr bool empty() const noexcept { return iovs_.empty(); }

    /**
     * @brief Total byte count across all segments.  O(n) in segment count.
     */
    [[nodiscard]] std::size_t total_bytes() const noexcept {
        std::size_t total = 0;
        for (const auto& iov : iovs_) total += iov.iov_len;
        return total;
    }

    // ─── Element access ────────────────────────────────────────────────────────

    /**
     * @brief Returns the i-th segment as a `std::span<const std::byte>`.
     * @pre `i < size()`
     */
    [[nodiscard]] std::span<const std::byte> operator[](std::size_t i) const noexcept {
        assert(i < iovs_.size() && "scattered_span index out of range");
        return {static_cast<const std::byte*>(iovs_[i].iov_base), iovs_[i].iov_len};
    }

    /**
     * @brief Returns the first segment.
     * @pre `!empty()`
     */
    [[nodiscard]] std::span<const std::byte> front() const noexcept {
        assert(!empty() && "scattered_span::front() on empty span");
        return (*this)[0];
    }

    /**
     * @brief Returns the last segment.
     * @pre `!empty()`
     */
    [[nodiscard]] std::span<const std::byte> back() const noexcept {
        assert(!empty() && "scattered_span::back() on empty span");
        return (*this)[size() - 1];
    }

    // ─── Range interface ───────────────────────────────────────────────────────

    [[nodiscard]] iterator begin() const noexcept { return {iovs_.data()}; }
    [[nodiscard]] iterator end()   const noexcept { return {iovs_.data() + iovs_.size()}; }

    [[nodiscard]] iterator cbegin() const noexcept { return begin(); }
    [[nodiscard]] iterator cend()   const noexcept { return end(); }

    // ─── Subrange ─────────────────────────────────────────────────────────────

    /**
     * @brief Returns a sub-view of segments `[offset, offset + count)`.
     *
     * Equivalent to `std::span::subspan`.
     *
     * @param offset  Starting segment index.
     * @param count   Number of segments; `std::dynamic_extent` means "until the end".
     * @pre `offset <= size()`
     */
    [[nodiscard]] scattered_span subspan(
        std::size_t offset,
        std::size_t count = std::dynamic_extent) const noexcept
    {
        return scattered_span{iovs_.subspan(offset, count)};
    }

private:
    std::span<const iovec> iovs_{};
};

// ─── make_scattered_span ──────────────────────────────────────────────────────

/**
 * @brief Build a `scattered_span` from a parameter pack of `BufferView` segments.
 *
 * Writes iovecs into the caller-supplied `IOVec<N>` (stack storage) and returns
 * a `scattered_span` view over it.  Zero allocation; the span is valid as long
 * as both `storage` and the underlying buffers remain in scope.
 *
 * ### Usage
 * @code
 * IOVec<4> vec;
 * auto scatter = make_scattered_span(vec,
 *     BufferView{header.data(), header.size()},
 *     BufferView{body.data(),   body.size()});
 * co_await stream.writev(scatter);
 * @endcode
 *
 * @tparam N  Storage capacity — must be >= the number of views supplied.
 * @param storage  Stack-allocated `IOVec<N>` that will hold the iovecs.
 *                 Its `count` is reset to 0 before the segments are written.
 * @param views    Buffer views to scatter over; at most N.
 */
template<std::size_t N, typename... Views>
    requires (sizeof...(Views) <= N) &&
             (std::convertible_to<Views, BufferView> && ...)
[[nodiscard]] scattered_span make_scattered_span(IOVec<N>& storage, Views&&... views) noexcept {
    storage.clear();
    (storage.push(static_cast<BufferView>(std::forward<Views>(views))), ...);
    return scattered_span{storage};
}

} // namespace qbuem

// ─── IOVec<N>::as_scattered() out-of-line definition ─────────────────────────
//
// Declared in iovec.hpp (with a forward declaration of scattered_span) and
// defined here so that including scattered_span.hpp makes it available.
// The template means the linker sees one definition per translation unit
// (ODR-safe via `inline`).

namespace qbuem {

template<std::size_t N>
inline scattered_span IOVec<N>::as_scattered() const noexcept {
    return scattered_span{as_const_span()};
}

} // namespace qbuem

/** @} */ // end of qbuem_scattered_span
