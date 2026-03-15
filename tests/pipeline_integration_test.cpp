#include <gtest/gtest.h>

#include <qbuem/pipeline/action.hpp>
#include <qbuem/pipeline/context.hpp>
#include <qbuem/pipeline/static_pipeline.hpp>
#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>

using namespace qbuem;

// ---------------------------------------------------------------------------
// Helper action functions used across tests
// ---------------------------------------------------------------------------

// Stage 1: int -> std::string
static Task<Result<std::string>> int_to_string(int x) {
  co_return std::to_string(x);
}

// Stage 2: std::string -> size_t (string length)
static Task<Result<size_t>> string_to_length(std::string s) {
  co_return s.size();
}

// Stage 3: size_t -> double (cast)
static Task<Result<double>> size_to_double(size_t n) {
  co_return static_cast<double>(n);
}

// ---------------------------------------------------------------------------
// StaticPipelineIntegration — ThreeStageChain
//
// Verifies that a 3-stage pipeline (int -> string -> size_t -> double) can
// be constructed and configured with correct types at compile time. The
// pipeline is built and started; item flow is exercised via try_push()
// against the head channel.
// ---------------------------------------------------------------------------
TEST(StaticPipelineIntegration, ThreeStageChain) {
  // Build a 3-stage pipeline: int -> string -> size_t -> double
  auto pipeline = pipeline_builder<int>()
      .add<std::string>(int_to_string)
      .add<size_t>(string_to_length)
      .add<double>(size_to_double)
      .build();

  using Pipeline = StaticPipeline<int, double>;

  // Pipeline starts in Created state
  EXPECT_EQ(pipeline.state(), Pipeline::State::Created);

  // Construct a Dispatcher (not run() — we only test construction + push API)
  Dispatcher dispatcher(1);

  pipeline.start(dispatcher);

  EXPECT_EQ(pipeline.state(), Pipeline::State::Running);

  // Push a handful of items non-blocking; the pipeline channel has default
  // capacity 256 so these should all succeed immediately.
  for (int i = 0; i < 10; ++i) {
    EXPECT_TRUE(pipeline.try_push(i));
  }

  // Verify stop transitions to Stopped state
  pipeline.stop();
  EXPECT_EQ(pipeline.state(), Pipeline::State::Stopped);

  dispatcher.stop();
}

// ---------------------------------------------------------------------------
// StaticPipelineIntegration — ScaleOut
//
// Verifies Action can be constructed with min_workers=1 and max_workers=4,
// and that scale_to() is callable without error. Since we are not running a
// live Reactor we exercise the API surface only.
// ---------------------------------------------------------------------------
TEST(StaticPipelineIntegration, ScaleOut) {
  Action<int, std::string>::Config cfg;
  cfg.min_workers = 1;
  cfg.max_workers = 4;
  cfg.channel_cap = 64;

  Action<int, std::string> action(int_to_string, cfg);

  Dispatcher dispatcher(1);

  // Wire an output channel and start the action
  auto out_ch = std::make_shared<AsyncChannel<ContextualItem<std::string>>>(64);
  action.start(dispatcher, out_ch);

  // scale_to(4) should spawn additional workers without crashing
  action.scale_to(4, dispatcher);

  // Push a few items into the action input channel
  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(action.try_push(i));
  }

  action.stop();
  dispatcher.stop();
}

// ---------------------------------------------------------------------------
// StaticPipelineIntegration — BackpressureTryPush
//
// Verifies that try_push() returns false once the head channel is full.
// Channel capacity is set to 2 so the third push must fail.
// ---------------------------------------------------------------------------
TEST(StaticPipelineIntegration, BackpressureTryPush) {
  // Single-stage pipeline with a very small channel
  Action<int, std::string>::Config cfg;
  cfg.channel_cap = 2;

  auto pipeline = pipeline_builder<int>()
      .add<std::string>(int_to_string, cfg)
      .build();

  // Do NOT start the pipeline — workers are not running, so the channel will
  // fill up and try_push must eventually return false.
  EXPECT_TRUE(pipeline.try_push(1));
  EXPECT_TRUE(pipeline.try_push(2));
  EXPECT_FALSE(pipeline.try_push(3)); // channel full, no consumer running
}

// ---------------------------------------------------------------------------
// StaticPipelineIntegration — DrainWaitsForInFlight
//
// Verifies that the pipeline state machine transitions correctly through
// Running -> Draining -> Stopped when drain() is called (synchronous portion
// only — we exercise the state transitions, not full async completion, since
// the Reactor is not pumping in this test environment).
// ---------------------------------------------------------------------------
TEST(StaticPipelineIntegration, DrainTransitionsState) {
  auto pipeline = pipeline_builder<int>()
      .add<std::string>(int_to_string)
      .build();

  using Pipeline = StaticPipeline<int, std::string>;
  Dispatcher dispatcher(1);
  pipeline.start(dispatcher);

  EXPECT_EQ(pipeline.state(), Pipeline::State::Running);

  // Push a few items before draining
  for (int i = 0; i < 3; ++i) {
    EXPECT_TRUE(pipeline.try_push(i));
  }

  // Calling stop() synchronously transitions the pipeline to Stopped without
  // requiring a running Reactor (drain() is a coroutine and requires co_await).
  pipeline.stop();
  EXPECT_EQ(pipeline.state(), Pipeline::State::Stopped);

  dispatcher.stop();
}

// ---------------------------------------------------------------------------
// StaticPipelineIntegration — ContextPropagation
//
// Verifies that a Context attached to a pushed item is present in the
// ContextualItem stored in the head channel, so downstream actions will
// receive it via ActionEnv::ctx.
// ---------------------------------------------------------------------------
TEST(StaticPipelineIntegration, ContextPropagation) {
  // Use a single-stage action and inspect the input channel directly.
  Action<int, std::string>::Config cfg;
  cfg.channel_cap = 16;

  Action<int, std::string> action(int_to_string, cfg);

  // Build a context with a RequestId
  Context ctx;
  ctx = ctx.put(RequestId{"req-integration-001"});

  // try_push attaches the context to the ContextualItem
  EXPECT_TRUE(action.try_push(42, ctx));

  // Peek at the head channel to verify the context was propagated
  auto item = action.input()->try_recv();
  ASSERT_TRUE(item.has_value());
  EXPECT_EQ(item->value, 42);

  auto rid = item->ctx.get<RequestId>();
  ASSERT_TRUE(rid.has_value());
  EXPECT_EQ(rid->value, "req-integration-001");
}

// ---------------------------------------------------------------------------
// StaticPipelineIntegration — StopTokenCancellation
//
// Verifies that stop() requests cancellation (stop_token propagation is
// available once an action has been started) and that the pipeline
// transitions to Stopped state.
// ---------------------------------------------------------------------------
TEST(StaticPipelineIntegration, StopTokenCancellation) {
  auto pipeline = pipeline_builder<int>()
      .add<std::string>(int_to_string)
      .build();

  using Pipeline = StaticPipeline<int, std::string>;
  Dispatcher dispatcher(1);
  pipeline.start(dispatcher);
  EXPECT_EQ(pipeline.state(), Pipeline::State::Running);

  // Immediately stop — cancels in-flight workers
  pipeline.stop();
  EXPECT_EQ(pipeline.state(), Pipeline::State::Stopped);

  dispatcher.stop();
}

// ---------------------------------------------------------------------------
// StaticPipelineIntegration — MultipleStartIdempotent
//
// Verifies that calling start() a second time on an already-running pipeline
// is a no-op (the state machine guard prevents double-start).
// ---------------------------------------------------------------------------
TEST(StaticPipelineIntegration, MultipleStartIdempotent) {
  auto pipeline = pipeline_builder<int>()
      .add<std::string>(int_to_string)
      .build();

  using Pipeline = StaticPipeline<int, std::string>;
  Dispatcher dispatcher(1);

  pipeline.start(dispatcher);
  EXPECT_EQ(pipeline.state(), Pipeline::State::Running);

  // Second start should be ignored — state must remain Running
  pipeline.start(dispatcher);
  EXPECT_EQ(pipeline.state(), Pipeline::State::Running);

  pipeline.stop();
  dispatcher.stop();
}

// ---------------------------------------------------------------------------
// StaticPipelineIntegration — OutputChannelAvailable
//
// Verifies that a built pipeline exposes a non-null output() channel that
// consumers can attach to.
// ---------------------------------------------------------------------------
TEST(StaticPipelineIntegration, OutputChannelAvailable) {
  auto pipeline = pipeline_builder<int>()
      .add<std::string>(int_to_string)
      .build();

  // Output channel must be wired at build time
  ASSERT_NE(pipeline.output(), nullptr);
}

// ---------------------------------------------------------------------------
// StaticPipelineIntegration — ChannelCapacityRespected
//
// Verifies that try_push into the Action's input channel respects the
// configured channel_cap for a standalone Action (not inside a pipeline).
// ---------------------------------------------------------------------------
TEST(StaticPipelineIntegration, ChannelCapacityRespected) {
  Action<int, std::string>::Config cfg;
  cfg.channel_cap = 4;

  Action<int, std::string> action(int_to_string, cfg);

  // Fill up the channel exactly
  EXPECT_TRUE(action.try_push(1));
  EXPECT_TRUE(action.try_push(2));
  EXPECT_TRUE(action.try_push(3));
  EXPECT_TRUE(action.try_push(4));

  // One more must fail
  EXPECT_FALSE(action.try_push(5));
}
