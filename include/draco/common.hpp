#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

namespace draco {

// Use C++23 std::expected for error handling instead of exceptions
template <typename T> using Result = std::expected<T, std::error_code>;

// Use std::span for zero-copy buffer views
using BufferView = std::span<const uint8_t>;
using MutableBufferView = std::span<uint8_t>;

} // namespace draco
