#pragma once

/**
 * @file qbuem/tracing/sampler.hpp
 * @brief Pluggable Sampler interface and built-in implementations.
 * @defgroup qbuem_sampler Sampler
 * @ingroup qbuem_tracing
 *
 * ## Design
 * The `Sampler` interface makes a sampling decision at the start of each span.
 *
 * ## Provided implementations
 * - `AlwaysSampler`        — always samples (development/debug)
 * - `NeverSampler`         — always drops (disables tracing with zero overhead)
 * - `ProbabilitySampler`   — probability-based sampling in the range [0.0, 1.0]
 * - `RateLimitingSampler`  — token-bucket-based maximum samples per second
 * - `ParentBasedSampler`   — follows the sampled flag of the parent span
 *
 * ## Usage example
 * ```cpp
 * auto sampler = std::make_shared<qbuem::tracing::ProbabilitySampler>(0.1);
 * qbuem::tracing::PipelineTracer::global().set_sampler(sampler);
 * ```
 * @{
 */

#include <qbuem/tracing/trace_context.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <random>
#include <string_view>

namespace qbuem::tracing {

// ---------------------------------------------------------------------------
// SamplingDecision
// ---------------------------------------------------------------------------

/**
 * @brief Sampling decision enumeration.
 */
enum class SamplingDecision {
  DROP,             ///< Do not sample this span.
  RECORD_AND_SAMPLE ///< Record and sample this span.
};

// ---------------------------------------------------------------------------
// SamplingContext — information required to make a sampling decision
// ---------------------------------------------------------------------------

/**
 * @brief Context provided to the Sampler at sampling time.
 */
struct SamplingContext {
  std::string_view pipeline_name;  ///< Pipeline name
  std::string_view action_name;    ///< Action name
  std::string_view span_name;      ///< Span name
  const TraceContext *parent;      ///< Parent TraceContext (nullptr for root)
};

// ---------------------------------------------------------------------------
// Sampler — pure virtual interface
// ---------------------------------------------------------------------------

/**
 * @brief Span sampling decision interface.
 */
class Sampler {
public:
  virtual ~Sampler() = default;

  /**
   * @brief Decides whether to sample the given context.
   * @param ctx Sampling context.
   * @returns RECORD_AND_SAMPLE or DROP.
   */
  [[nodiscard]] virtual SamplingDecision should_sample(
      const SamplingContext &ctx) noexcept = 0;

  /// @brief Description string for this Sampler.
  [[nodiscard]] virtual std::string_view description() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// AlwaysSampler
// ---------------------------------------------------------------------------

/**
 * @brief Always samples. Suitable for development/debug environments.
 */
class AlwaysSampler final : public Sampler {
public:
  [[nodiscard]] SamplingDecision should_sample(
      const SamplingContext &) noexcept override {
    return SamplingDecision::RECORD_AND_SAMPLE;
  }

  [[nodiscard]] std::string_view description() const noexcept override {
    return "AlwaysSampler";
  }
};

// ---------------------------------------------------------------------------
// NeverSampler
// ---------------------------------------------------------------------------

/**
 * @brief Never samples. Zero-overhead when tracing is disabled.
 */
class NeverSampler final : public Sampler {
public:
  [[nodiscard]] SamplingDecision should_sample(
      const SamplingContext &) noexcept override {
    return SamplingDecision::DROP;
  }

  [[nodiscard]] std::string_view description() const noexcept override {
    return "NeverSampler";
  }
};

// ---------------------------------------------------------------------------
// ProbabilitySampler
// ---------------------------------------------------------------------------

/**
 * @brief Probability-based sampler in the range [0.0, 1.0].
 *
 * `rate = 0.1` → 10% sampling, `rate = 1.0` → 100% sampling.
 * Uses per-thread RNG for lock-free operation.
 */
class ProbabilitySampler final : public Sampler {
public:
  /**
   * @param rate Sampling probability [0.0, 1.0]. Values outside the range are clamped.
   */
  explicit ProbabilitySampler(double rate) noexcept
      : rate_(std::clamp(rate, 0.0, 1.0)) {}

  [[nodiscard]] SamplingDecision should_sample(
      const SamplingContext &) noexcept override {
    if (rate_ <= 0.0) return SamplingDecision::DROP;
    if (rate_ >= 1.0) return SamplingDecision::RECORD_AND_SAMPLE;
    thread_local std::mt19937_64 rng{std::random_device{}()};
    thread_local std::uniform_real_distribution<double> dist{0.0, 1.0};
    return (dist(rng) < rate_) ? SamplingDecision::RECORD_AND_SAMPLE
                                : SamplingDecision::DROP;
  }

  [[nodiscard]] std::string_view description() const noexcept override {
    return "ProbabilitySampler";
  }

  [[nodiscard]] double rate() const noexcept { return rate_; }

private:
  double rate_;
};

// ---------------------------------------------------------------------------
// RateLimitingSampler
// ---------------------------------------------------------------------------

/**
 * @brief Token bucket 기반 초당 최대 샘플 수 제한 샘플러.
 *
 * `max_per_second = 100` → 초당 최대 100개 스팬만 샘플링.
 * 버스트 허용량은 `max_per_second`와 동일합니다.
 */
class RateLimitingSampler final : public Sampler {
public:
  /**
   * @param max_per_second 초당 최대 샘플 수. 0이면 NeverSampler와 동일.
   */
  explicit RateLimitingSampler(double max_per_second)
      : rate_(max_per_second),
        tokens_(max_per_second),
        last_refill_(clock::now()) {}

  [[nodiscard]] SamplingDecision should_sample(
      const SamplingContext &) noexcept override {
    if (rate_ <= 0.0) return SamplingDecision::DROP;

    std::lock_guard<std::mutex> lock(mutex_);
    auto now = clock::now();
    double elapsed = std::chrono::duration<double>(now - last_refill_).count();

    // 토큰 보충
    tokens_ = std::min(rate_, tokens_ + elapsed * rate_);
    last_refill_ = now;

    if (tokens_ >= 1.0) {
      tokens_ -= 1.0;
      return SamplingDecision::RECORD_AND_SAMPLE;
    }
    return SamplingDecision::DROP;
  }

  [[nodiscard]] std::string_view description() const noexcept override {
    return "RateLimitingSampler";
  }

private:
  using clock = std::chrono::steady_clock;

  double           rate_;
  double           tokens_;
  clock::time_point last_refill_;
  mutable std::mutex mutex_;
};

// ---------------------------------------------------------------------------
// ParentBasedSampler
// ---------------------------------------------------------------------------

/**
 * @brief 부모 스팬의 sampled 플래그를 따르는 샘플러.
 *
 * - 부모가 있고 sampled=1 → RECORD_AND_SAMPLE
 * - 부모가 있고 sampled=0 → DROP
 * - 부모 없음 (루트) → `root_sampler`에 위임
 */
class ParentBasedSampler final : public Sampler {
public:
  /**
   * @param root_sampler 루트 스팬(부모 없음)에 적용할 샘플러.
   *                     nullptr이면 AlwaysSampler 사용.
   */
  explicit ParentBasedSampler(std::shared_ptr<Sampler> root_sampler = nullptr)
      : root_(root_sampler
                  ? std::move(root_sampler)
                  : std::make_shared<AlwaysSampler>()) {}

  [[nodiscard]] SamplingDecision should_sample(
      const SamplingContext &ctx) noexcept override {
    if (ctx.parent == nullptr) {
      return root_->should_sample(ctx);
    }
    // W3C trace-flags: bit 0 = sampled
    bool parent_sampled = (ctx.parent->flags & 0x01) != 0;
    return parent_sampled ? SamplingDecision::RECORD_AND_SAMPLE
                          : SamplingDecision::DROP;
  }

  [[nodiscard]] std::string_view description() const noexcept override {
    return "ParentBasedSampler";
  }

private:
  std::shared_ptr<Sampler> root_;
};

} // namespace qbuem::tracing

/** @} */
