#pragma once

/**
 * @file qbuem/io/direct_file.hpp
 * @brief O_DIRECT file I/O with 512-byte aligned buffers for bypass of the page cache.
 * @defgroup qbuem_io_direct DirectFile
 * @ingroup qbuem_io
 *
 * ## Design (v2.4.0)
 * `DirectFile` opens a file with `O_DIRECT | O_DSYNC` and performs offset-based
 * reads/writes using buffers that are guaranteed to be sector-aligned (512 B
 * minimum, configurable up to 4 KiB for 4Kn drives).
 *
 * ### Why O_DIRECT
 * - Bypasses the Linux page cache: eliminates "double buffering".
 * - Provides predictable latency by avoiding kernel writeback spikes.
 * - Required for database-grade WAL / log-structured storage.
 *
 * ### Alignment Rules
 * All three quantities must be aligned to the logical block size (typically 512 B):
 *   1. The memory buffer address.
 *   2. The file offset.
 *   3. The transfer size.
 *
 * `AlignedBuffer` satisfies rule 1. Callers must satisfy rules 2 and 3.
 *
 * ### FileSink / FileSource Pipeline Adapters
 * `FileSink<T>` and `FileSource<T>` integrate `DirectFile` into the
 * qbuem pipeline API (`with_sink()` / `with_source()`).
 *
 * @code
 * // Write pipeline output directly to disk — zero page-cache involvement.
 * auto pipeline = PipelineBuilder<RawRecord, DiskRecord>{}
 *     .add<DiskRecord>(serialize_stage)
 *     .with_sink(FileSink<DiskRecord>("wal.bin"))
 *     .build();
 *
 * // Read from disk into pipeline.
 * auto pipeline2 = PipelineBuilder<DiskRecord, ProcessedRecord>{}
 *     .with_source(FileSource<DiskRecord>("wal.bin"))
 *     .add<ProcessedRecord>(deserialize_stage)
 *     .build();
 * @endcode
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>

#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace qbuem {

// ─── AlignedBuffer ────────────────────────────────────────────────────────────

/**
 * @brief Heap-allocated buffer aligned to `Align` bytes.
 *
 * Required for O_DIRECT — the kernel validates buffer alignment and returns
 * EINVAL when it is not a multiple of the logical block size.
 *
 * @tparam Align Alignment in bytes (must be a power of two, ≥ 512).
 */
template <size_t Align = 512>
class AlignedBuffer {
    static_assert((Align & (Align - 1)) == 0, "Align must be a power of two");
    static_assert(Align >= 512, "O_DIRECT requires at least 512-byte alignment");

public:
    /**
     * @brief Allocate an aligned buffer of `size` bytes.
     * @param size Buffer size in bytes. Rounded up to the nearest `Align` multiple.
     */
    explicit AlignedBuffer(size_t size)
        : size_(round_up(size))
        , data_(static_cast<std::byte*>(
              ::operator new(size_, std::align_val_t{Align})))
    {
        std::memset(data_, 0, size_);
    }

    ~AlignedBuffer() noexcept {
        ::operator delete(data_, std::align_val_t{Align});
    }

    AlignedBuffer(const AlignedBuffer&)            = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;

    AlignedBuffer(AlignedBuffer&& o) noexcept
        : size_(o.size_), data_(o.data_) {
        o.data_ = nullptr;
        o.size_ = 0;
    }

    [[nodiscard]] std::byte*       data()  noexcept { return data_; }
    [[nodiscard]] const std::byte* data()  const noexcept { return data_; }
    [[nodiscard]] size_t           size()  const noexcept { return size_; }

    [[nodiscard]] std::span<std::byte>       span()       noexcept { return {data_, size_}; }
    [[nodiscard]] std::span<const std::byte> span() const noexcept { return {data_, size_}; }

private:
    static constexpr size_t round_up(size_t n) noexcept {
        return (n + Align - 1) & ~(Align - 1);
    }

    size_t      size_;
    std::byte*  data_;
};

// ─── DirectFile ───────────────────────────────────────────────────────────────

/**
 * @brief O_DIRECT file handle with offset-based read/write.
 *
 * All I/O is synchronous and blocking (suitable for offloading to a dedicated
 * I/O thread). For io_uring-based async O_DIRECT, see `uring_ops.hpp`.
 */
class DirectFile {
public:
    DirectFile() noexcept : fd_(-1) {}
    ~DirectFile() noexcept { close(); }

    DirectFile(const DirectFile&)            = delete;
    DirectFile& operator=(const DirectFile&) = delete;
    DirectFile(DirectFile&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }

    /**
     * @brief Open a file with O_DIRECT.
     *
     * @param path       File path.
     * @param write      If true, open for read-write (creates file if absent).
     *                   If false, open read-only.
     * @returns DirectFile on success, or an error Result.
     */
    [[nodiscard]] static Result<DirectFile> open(std::string_view path,
                                                  bool write = false) noexcept {
        // O_DIRECT: bypass page cache.
        // O_DSYNC:  data and required metadata written to storage on each write.
        const int flags = write
            ? (O_RDWR | O_CREAT | O_DIRECT | O_DSYNC)
            : (O_RDONLY | O_DIRECT);

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        const int fd = ::open(path.data(), flags, 0644);
        if (fd < 0)
            return unexpected(std::error_code{errno, std::system_category()});

        DirectFile f;
        f.fd_ = fd;
        return f;
    }

    /**
     * @brief Read up to `buf.size()` bytes from `offset`.
     *
     * @param buf    Destination — must be 512-byte aligned (use AlignedBuffer).
     * @param offset File offset — must be a 512-byte multiple.
     * @returns Number of bytes actually read, or an error.
     */
    [[nodiscard]] Result<size_t> read_at(std::span<std::byte>  buf,
                                          off_t                 offset) const noexcept {
        assert(reinterpret_cast<uintptr_t>(buf.data()) % 512 == 0 &&
               "buffer must be 512-byte aligned for O_DIRECT");
        assert(offset % 512 == 0 &&
               "offset must be a 512-byte multiple for O_DIRECT");
        assert(buf.size() % 512 == 0 &&
               "size must be a 512-byte multiple for O_DIRECT");

        const ssize_t n = ::pread(fd_, buf.data(), buf.size(), offset);
        if (n < 0)
            return unexpected(std::error_code{errno, std::system_category()});
        return static_cast<size_t>(n);
    }

    /**
     * @brief Write exactly `buf.size()` bytes at `offset`.
     *
     * @param buf    Source data — must be 512-byte aligned (use AlignedBuffer).
     * @param offset File offset — must be a 512-byte multiple.
     * @returns Number of bytes written, or an error.
     */
    [[nodiscard]] Result<size_t> write_at(std::span<const std::byte> buf,
                                           off_t                      offset) noexcept {
        assert(reinterpret_cast<uintptr_t>(buf.data()) % 512 == 0 &&
               "buffer must be 512-byte aligned for O_DIRECT");
        assert(offset % 512 == 0 &&
               "offset must be a 512-byte multiple for O_DIRECT");
        assert(buf.size() % 512 == 0 &&
               "size must be a 512-byte multiple for O_DIRECT");

        size_t written = 0;
        while (written < buf.size()) {
            const ssize_t n = ::pwrite(fd_,
                buf.data() + written,
                buf.size() - written,
                offset + static_cast<off_t>(written));
            if (n < 0) {
                if (errno == EINTR) continue;
                return unexpected(std::error_code{errno, std::system_category()});
            }
            written += static_cast<size_t>(n);
        }
        return written;
    }

    /** @brief Close the file descriptor. Idempotent. */
    void close() noexcept {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    [[nodiscard]] bool  valid() const noexcept { return fd_ >= 0; }
    [[nodiscard]] int   fd()    const noexcept { return fd_; }

private:
    int fd_;
};

// ─── FileSink<T> ──────────────────────────────────────────────────────────────

/**
 * @brief Pipeline sink that serializes `T` objects to disk via O_DIRECT.
 *
 * `T` must be trivially copyable (no hidden allocations, deterministic size).
 * Each `sink()` call appends one `T` at the next 512-byte-aligned offset.
 *
 * Conforms to the `ISink<T>` duck-typed interface expected by
 * `PipelineBuilder::with_sink()`.
 *
 * @tparam T Trivially-copyable record type.
 */
template <typename T>
    requires std::is_trivially_copyable_v<T>
class FileSink {
    // Stride is rounded up to 512 bytes for O_DIRECT alignment.
    static constexpr size_t kStride = (sizeof(T) + 511) & ~size_t{511};

public:
    /**
     * @brief Construct and open the sink file for writing.
     * @param path Output file path (created if absent).
     */
    explicit FileSink(std::string_view path)
        : path_(path)
        , buf_(kStride)
    {}

    /** @brief Open the backing file. Called by the pipeline framework. */
    [[nodiscard]] Result<void> init() noexcept {
        auto res = DirectFile::open(path_, /*write=*/true);
        if (!res) return unexpected(res.error());
        file_ = std::move(*res);
        offset_ = 0;
        return Result<void>::ok();
    }

    /**
     * @brief Write one record to disk.
     * @param item Record to persist.
     * @returns Result<void> — error on I/O failure.
     */
    [[nodiscard]] Result<void> sink(const T& item) noexcept {
        std::memcpy(buf_.data(), &item, sizeof(T));
        // Zero-pad the remainder of the stride.
        if constexpr (sizeof(T) < kStride)
            std::memset(buf_.data() + sizeof(T), 0, kStride - sizeof(T));

        auto res = file_.write_at(buf_.span(), offset_);
        if (!res) return unexpected(res.error());
        offset_ += static_cast<off_t>(kStride);
        return Result<void>::ok();
    }

    /** @brief Flush and close the backing file. */
    void finish() noexcept { file_.close(); }

private:
    std::string  path_;
    DirectFile   file_;
    AlignedBuffer<512> buf_;
    off_t        offset_{0};
};

// ─── FileSource<T> ────────────────────────────────────────────────────────────

/**
 * @brief Pipeline source that reads `T` objects from disk via O_DIRECT.
 *
 * `T` must be trivially copyable. Each `next()` call returns the next record
 * or `std::nullopt` at end-of-file.
 *
 * Conforms to the `ISource<T>` duck-typed interface expected by
 * `PipelineBuilder::with_source()`.
 *
 * @tparam T Trivially-copyable record type.
 */
template <typename T>
    requires std::is_trivially_copyable_v<T>
class FileSource {
    static constexpr size_t kStride = (sizeof(T) + 511) & ~size_t{511};

public:
    explicit FileSource(std::string_view path)
        : path_(path)
        , buf_(kStride)
    {}

    [[nodiscard]] Result<void> init() noexcept {
        auto res = DirectFile::open(path_, /*write=*/false);
        if (!res) return unexpected(res.error());
        file_ = std::move(*res);
        offset_ = 0;
        return Result<void>::ok();
    }

    /**
     * @brief Read the next record.
     * @returns The next T, or std::nullopt at EOF, or an error.
     */
    [[nodiscard]] Result<std::optional<T>> next() noexcept {
        auto res = file_.read_at(buf_.span(), offset_);
        if (!res) return unexpected(res.error());
        if (*res == 0) return std::optional<T>{std::nullopt}; // EOF

        T item{};
        std::memcpy(&item, buf_.data(), sizeof(T));
        offset_ += static_cast<off_t>(kStride);
        return std::optional<T>{item};
    }

    void finish() noexcept { file_.close(); }

private:
    std::string        path_;
    DirectFile         file_;
    AlignedBuffer<512> buf_;
    off_t              offset_{0};
};

} // namespace qbuem
