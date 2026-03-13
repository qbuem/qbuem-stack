#pragma once

/**
 * @file draco/version.hpp
 * @brief qbuem-stack 라이브러리의 버전 정보를 정의합니다.
 * @ingroup qbuem_version
 *
 * 이 헤더는 컴파일 타임 버전 상수와 전처리기 매크로를 모두 제공합니다.
 * - 템플릿/constexpr 코드에서는 `draco::Version::major` 등의 상수를 사용하세요.
 * - 전처리기 조건 분기(`#if`)가 필요한 경우에는 매크로(`DRACO_VERSION_MAJOR`)를 사용하세요.
 *
 * ### 버전 이력
 * - 0.1.0: 최초 공개 릴리스
 * - 0.2.0: 비동기 로거(AsyncLogger), 코루틴 Task 추가
 * - 0.4.0: AsyncConnect awaiter, FixedPoolResource, 전반적 API 안정화
 */

/**
 * @defgroup qbuem_version Version
 * @brief 라이브러리 버전 식별 심볼 모음.
 *
 * 런타임 및 컴파일 타임 양쪽에서 버전을 조회할 수 있도록
 * 구조체 상수(constexpr)와 C 매크로 두 가지 형태로 제공됩니다.
 * Semantic Versioning 2.0.0(https://semver.org)을 따릅니다.
 * @{
 */

#include <string_view>

namespace draco {

/**
 * @brief qbuem-stack 라이브러리 버전 정보를 담는 구조체.
 *
 * 모든 멤버는 `constexpr`이므로 컴파일 타임 상수로 사용할 수 있습니다.
 * static_assert나 템플릿 파라미터에서도 활용 가능합니다.
 *
 * @note Semantic Versioning 규칙:
 *   - `major`: 하위 호환이 깨지는 API 변경 시 증가
 *   - `minor`: 하위 호환을 유지하면서 기능 추가 시 증가
 *   - `patch`: 버그 수정만 있을 경우 증가
 *
 * @code
 * static_assert(draco::Version::major >= 0, "버전 확인");
 * std::cout << draco::Version::string << '\n'; // "0.4.0"
 * @endcode
 */
struct Version {
  /** @brief Major 버전 번호. API 하위 호환이 깨질 때 증가합니다. */
  static constexpr int major = 0;

  /** @brief Minor 버전 번호. 하위 호환을 유지하며 새 기능이 추가될 때 증가합니다. */
  static constexpr int minor = 4;

  /** @brief Patch 버전 번호. 버그 수정만 이루어질 때 증가합니다. */
  static constexpr int patch = 0;

  /** @brief "major.minor.patch" 형식의 버전 문자열 (null-terminated 보장). */
  static constexpr std::string_view string = "0.4.0";
};

} // namespace draco

/** @brief Major 버전 번호 (전처리기 조건 분기용). */
#define DRACO_VERSION_MAJOR 0

/** @brief Minor 버전 번호 (전처리기 조건 분기용). */
#define DRACO_VERSION_MINOR 4

/** @brief Patch 버전 번호 (전처리기 조건 분기용). */
#define DRACO_VERSION_PATCH 0

/** @brief "major.minor.patch" 형식의 버전 문자열 리터럴 (전처리기 조건 분기용). */
#define DRACO_VERSION_STRING "0.4.0"

/** @} */ // end of qbuem_version
