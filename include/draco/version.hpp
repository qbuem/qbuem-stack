#pragma once

#include <string_view>

namespace draco {

/**
 * @brief Draco WAS Library Version
 */
struct Version {
  static constexpr int major = 0;
  static constexpr int minor = 2;
  static constexpr int patch = 0;

  static constexpr std::string_view string = "0.2.0";
};

#define DRACO_VERSION_MAJOR 0
#define DRACO_VERSION_MINOR 2
#define DRACO_VERSION_PATCH 0
#define DRACO_VERSION_STRING "0.2.0"

} // namespace draco
