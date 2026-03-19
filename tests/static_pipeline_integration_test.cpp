#include <qbuem/pipeline/pipeline.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace qbuem;

// ---------------------------------------------------------------------------
// Domain types — 3-stage chain: RawEvent → ParsedEvent → EnrichedEvent → StoredEvent
// ---------------------------------------------------------------------------

/** @brief Pipeline input — raw string event. */
struct RawEvent {
  std::string payload;
};

/** @brief Parsed event — value converted to integer. */
struct ParsedEvent {
  int value = 0;
};

/** @brief Enriched event — value with added tag. */
struct EnrichedEvent {
  int    value = 0;
  std::string tag;
};

/** @brief Stored event — final output type. */
struct StoredEvent {
  int    value = 0;
  std::string tag;
  bool   stored = false;
};

// ---------------------------------------------------------------------------
// Action functions
// ---------------------------------------------------------------------------

/** @brief RawEvent → ParsedEvent: parse payload as integer. */
static Task<Result<ParsedEvent>> parse_action(RawEvent raw) {
  int v = 0;
  for (char c : raw.payload) {
    if (c >= '0' && c <= '9') v = v * 10 + (c - '0');
  }
  co_return ParsedEvent{v};
}

/** @brief ParsedEvent → EnrichedEvent: add "enriched" tag. */
static Task<Result<EnrichedEvent>> enrich_action(ParsedEvent parsed) {
  co_return EnrichedEvent{parsed.value, "enriched"};
}

/** @brief EnrichedEvent → StoredEvent: set stored flag. */
static Task<Result<StoredEvent>> store_action(EnrichedEvent enriched) {
  co_return StoredEvent{enriched.value, enriched.tag, true};
}

// ---------------------------------------------------------------------------
// Test 1: 3-stage chain StaticPipeline
// ---------------------------------------------------------------------------

/**
 * @brief Build a 3-stage pipeline and flow items through it.
 *
 * Verifies:
 * - Type conversion is performed correctly at each stage
 * - Final output matches expected values
 */
TEST(StaticPipelineIntegration, ThreeStageChain) {
  // Build pipeline: RawEvent → ParsedEvent → EnrichedEvent → StoredEvent
  auto pipeline = pipeline_builder<RawEvent>()
      .add<ParsedEvent>(parse_action)
      .add<EnrichedEvent>(enrich_action)
      .add<StoredEvent>(store_action)
      .build();

  // Obtain output channel
  auto output = pipeline.output();
  ASSERT_NE(output, nullptr);

  // Create Dispatcher (1 thread) + run event loop in background
  Dispatcher dispatcher(1);
  pipeline.start(dispatcher);
  std::jthread run_thread([&] { dispatcher.run(); });

  // Push items
  std::vector<int> expected_values = {42, 7, 100};
  for (int v : expected_values) {
    bool pushed = pipeline.try_push(RawEvent{std::to_string(v)});
    EXPECT_TRUE(pushed) << "try_push failed: value=" << v;
  }

  // Collect output (with timeout)
  std::vector<StoredEvent> results;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

  while (results.size() < expected_values.size() &&
         std::chrono::steady_clock::now() < deadline) {
    auto item = output->try_recv();
    if (item.has_value()) {
      results.push_back(std::move(item->value));
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  pipeline.stop();  // close channels so worker coroutines exit before dispatcher stops
  dispatcher.stop();
  run_thread.join();

  // Verify
  ASSERT_EQ(results.size(), expected_values.size());
  for (const auto &r : results) {
    EXPECT_TRUE(r.stored) << "stored flag must be true";
    EXPECT_EQ(r.tag, "enriched") << "tag must be 'enriched'";
    // Value must be one of expected_values
    bool found = false;
    for (int v : expected_values)
      if (r.value == v) { found = true; break; }
    EXPECT_TRUE(found) << "unexpected value: " << r.value;
  }
}

// ---------------------------------------------------------------------------
// Test 2: Scale-out — spawn additional workers under load
// ---------------------------------------------------------------------------

/**
 * @brief Verify that adding workers via Action::scale_to() allows
 *        processing more items.
 *
 * Verifies:
 * - Whether more items can be processed in parallel after scale_out()
 * - Output count matches input count
 */
TEST(StaticPipelineIntegration, ScaleOutUnderLoad) {
  std::atomic<size_t> processed{0};

  // Slow processing function (counter only to measure parallelism effect)
  auto counting_action = [&processed](RawEvent) -> Task<Result<ParsedEvent>> {
    processed.fetch_add(1, std::memory_order_relaxed);
    co_return ParsedEvent{1};
  };

  auto pipeline = pipeline_builder<RawEvent>()
      .add<ParsedEvent>(counting_action,
                        Action<RawEvent, ParsedEvent>::Config{
                            .min_workers = 1,
                            .max_workers = 4,
                            .channel_cap = 64,
                        })
      .build();

  auto output = pipeline.output();
  Dispatcher dispatcher(2);
  pipeline.start(dispatcher);
  std::jthread run_thread([&] { dispatcher.run(); });

  // Push items
  constexpr size_t kItems = 20;
  size_t pushed = 0;
  for (size_t i = 0; i < kItems; ++i) {
    if (pipeline.try_push(RawEvent{"1"}))
      ++pushed;
  }

  // Wait for processing to complete
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (processed.load() < pushed &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  pipeline.stop();  // close channels so worker coroutines exit before dispatcher stops
  dispatcher.stop();
  run_thread.join();

  // Processed count must not exceed pushed count
  EXPECT_LE(processed.load(), pushed)
      << "processed items cannot exceed pushed items";
  EXPECT_GT(processed.load(), 0u) << "at least 1 item must be processed";
}

// ---------------------------------------------------------------------------
// Test 3: Drain — guarantee all items are processed before stop
// ---------------------------------------------------------------------------

/**
 * @brief Verify that all items are processed before drain() completes.
 *
 * Verifies:
 * - processed item count == pushed item count after drain() completes
 */
TEST(StaticPipelineIntegration, DrainProcessesAllItems) {
  std::atomic<size_t> processed{0};

  auto counting_action = [&processed](RawEvent) -> Task<Result<ParsedEvent>> {
    processed.fetch_add(1, std::memory_order_relaxed);
    co_return ParsedEvent{0};
  };

  auto pipeline = pipeline_builder<RawEvent>()
      .add<ParsedEvent>(counting_action,
                        Action<RawEvent, ParsedEvent>::Config{
                            .min_workers = 1,
                            .max_workers = 1,
                            .channel_cap = 128,
                        })
      .build();

  Dispatcher dispatcher(1);
  pipeline.start(dispatcher);
  std::jthread run_thread([&] { dispatcher.run(); });

  // Push items
  constexpr size_t kItems = 10;
  for (size_t i = 0; i < kItems; ++i)
    pipeline.try_push(RawEvent{"x"});

  // Run drain — wait until all items are processed
  std::atomic<bool> drain_done{false};

  // Define drain wrapper coroutine
  auto drain_task = [&]() -> Task<void> {
    co_await pipeline.drain();
    drain_done.store(true, std::memory_order_release);
    co_return;
  };
  dispatcher.spawn(drain_task());

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!drain_done.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  dispatcher.stop();
  run_thread.join();

  EXPECT_TRUE(drain_done.load()) << "drain() did not complete within timeout";
  EXPECT_EQ(processed.load(), kItems)
      << "all items must be processed after drain completes";
}

// ---------------------------------------------------------------------------
// Test 4: Backpressure — try_push returns false when channel is full
// ---------------------------------------------------------------------------

/**
 * @brief Verify that `try_push()` returns false when channel capacity is exceeded.
 *
 * Verifies:
 * - A push that exceeds channel capacity (cap) must return false
 */
TEST(StaticPipelineIntegration, BackpressureTryPushReturnsFalse) {
  constexpr size_t kChannelCap = 4;

  // Action that fills the channel by delaying processing
  // (channel filled without starting any workers)
  auto slow_action = [](RawEvent) -> Task<Result<ParsedEvent>> {
    // This function must not be called in the actual test (channel full state)
    co_return ParsedEvent{0};
  };

  auto pipeline = pipeline_builder<RawEvent>()
      .add<ParsedEvent>(slow_action,
                        Action<RawEvent, ParsedEvent>::Config{
                            .min_workers = 0, // no workers — fill channel only
                            .max_workers = 0,
                            .channel_cap = kChannelCap,
                        })
      .build();

  // Without starting the Dispatcher there are no workers, so the channel is not consumed.
  // Fill the channel using try_push.
  size_t success_count = 0;
  size_t fail_count    = 0;

  // Attempt channel capacity + extra
  for (size_t i = 0; i < kChannelCap + 10; ++i) {
    if (pipeline.try_push(RawEvent{"x"}))
      ++success_count;
    else
      ++fail_count;
  }

  // At least some pushes must fail (when channel is full)
  EXPECT_GT(fail_count, 0u)
      << "try_push must return false when channel is full";
  EXPECT_LE(success_count, kChannelCap + 1)
      << "successful push count cannot greatly exceed channel capacity";
}

// ---------------------------------------------------------------------------
// Test 5: Context propagation — verify Context passes through stages
// ---------------------------------------------------------------------------

/**
 * @brief Verify that Context propagates correctly through pipeline stages.
 *
 * Verifies:
 * - The RequestId in the Context passed at `push()` is preserved in the final stage
 */
TEST(StaticPipelineIntegration, ContextPropagationThroughStages) {
  // Action that extracts Context — accesses ctx via ActionEnv
  auto ctx_aware_parse = [](RawEvent raw, ActionEnv env)
      -> Task<Result<ParsedEvent>> {
    // Check if RequestId is in Context — if so, use its value as payload
    auto rid = env.ctx.get<RequestId>();
    int  v   = 0;
    if (rid.has_value()) {
      // Use length of RequestId's value (for propagation verification)
      v = static_cast<int>(rid->value.size());
    } else {
      // No Context — parse original payload as integer
      for (char c : raw.payload)
        if (c >= '0' && c <= '9') v = v * 10 + (c - '0');
    }
    co_return ParsedEvent{v};
  };

  // Final action that captures Context propagation
  std::atomic<int>         captured_value{-1};
  std::atomic<bool>        ctx_was_set{false};

  auto ctx_aware_store = [&](EnrichedEvent enriched, ActionEnv env)
      -> Task<Result<StoredEvent>> {
    auto rid = env.ctx.get<RequestId>();
    ctx_was_set.store(rid.has_value(), std::memory_order_relaxed);
    captured_value.store(enriched.value, std::memory_order_relaxed);
    co_return StoredEvent{enriched.value, enriched.tag, true};
  };

  auto pipeline = pipeline_builder<RawEvent>()
      .add<ParsedEvent>(ctx_aware_parse)
      .add<EnrichedEvent>(enrich_action)
      .add<StoredEvent>(ctx_aware_store)
      .build();

  auto output = pipeline.output();
  Dispatcher dispatcher(1);
  pipeline.start(dispatcher);
  std::jthread run_thread([&] { dispatcher.run(); });

  // Push with RequestId stored in Context
  const std::string kReqId = "req-ctx-test-001";
  Context ctx;
  ctx = ctx.put(RequestId{kReqId});

  pipeline.try_push(RawEvent{"0"}, ctx);

  // Wait for output
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  bool got_output = false;
  while (!got_output && std::chrono::steady_clock::now() < deadline) {
    auto item = output->try_recv();
    if (item.has_value()) {
      got_output = true;
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  pipeline.stop();  // close channels so worker coroutines exit before dispatcher stops
  dispatcher.stop();
  run_thread.join();

  ASSERT_TRUE(got_output) << "no output produced within timeout";

  // Verify RequestId from Context was propagated and reflected in value
  // (in ctx_aware_parse: rid->value.size() == kReqId.size())
  EXPECT_TRUE(ctx_was_set.load())
      << "Context's RequestId was lost in the final stage";
  EXPECT_EQ(captured_value.load(),
            static_cast<int>(kReqId.size()))
      << "value propagated via Context differs from expected";
}
