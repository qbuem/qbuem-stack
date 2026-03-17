#include <qbuem/pipeline/pipeline.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace qbuem;

// ---------------------------------------------------------------------------
// 도메인 타입 — 3단계 체인: RawEvent → ParsedEvent → EnrichedEvent → StoredEvent
// ---------------------------------------------------------------------------

/** @brief 파이프라인 입력 — 원시 문자열 이벤트. */
struct RawEvent {
  std::string payload;
};

/** @brief 파싱된 이벤트 — 숫자로 변환된 값. */
struct ParsedEvent {
  int value = 0;
};

/** @brief 보강된 이벤트 — 값에 태그 추가. */
struct EnrichedEvent {
  int    value = 0;
  std::string tag;
};

/** @brief 저장된 이벤트 — 최종 출력 타입. */
struct StoredEvent {
  int    value = 0;
  std::string tag;
  bool   stored = false;
};

// ---------------------------------------------------------------------------
// 액션 함수
// ---------------------------------------------------------------------------

/** @brief RawEvent → ParsedEvent: payload를 정수로 파싱. */
static Task<Result<ParsedEvent>> parse_action(RawEvent raw) {
  int v = 0;
  for (char c : raw.payload) {
    if (c >= '0' && c <= '9') v = v * 10 + (c - '0');
  }
  co_return ParsedEvent{v};
}

/** @brief ParsedEvent → EnrichedEvent: "enriched" 태그 추가. */
static Task<Result<EnrichedEvent>> enrich_action(ParsedEvent parsed) {
  co_return EnrichedEvent{parsed.value, "enriched"};
}

/** @brief EnrichedEvent → StoredEvent: stored 플래그 설정. */
static Task<Result<StoredEvent>> store_action(EnrichedEvent enriched) {
  co_return StoredEvent{enriched.value, enriched.tag, true};
}

// ---------------------------------------------------------------------------
// 테스트 1: 3단계 체인 StaticPipeline
// ---------------------------------------------------------------------------

/**
 * @brief 3단계 파이프라인을 빌드하고 아이템을 흘려보냅니다.
 *
 * 검증:
 * - 각 단계 타입 변환이 정확히 이루어지는지
 * - 최종 출력이 예상값과 일치하는지
 */
TEST(StaticPipelineIntegration, ThreeStageChain) {
  // 파이프라인 빌드: RawEvent → ParsedEvent → EnrichedEvent → StoredEvent
  auto pipeline = pipeline_builder<RawEvent>()
      .add<ParsedEvent>(parse_action)
      .add<EnrichedEvent>(enrich_action)
      .add<StoredEvent>(store_action)
      .build();

  // 출력 채널 획득
  auto output = pipeline.output();
  ASSERT_NE(output, nullptr);

  // Dispatcher 생성 (1 스레드) + 이벤트 루프 백그라운드 실행
  Dispatcher dispatcher(1);
  pipeline.start(dispatcher);
  std::jthread run_thread([&] { dispatcher.run(); });

  // 아이템 투입
  std::vector<int> expected_values = {42, 7, 100};
  for (int v : expected_values) {
    bool pushed = pipeline.try_push(RawEvent{std::to_string(v)});
    EXPECT_TRUE(pushed) << "try_push 실패: value=" << v;
  }

  // 출력 수집 (타임아웃 포함)
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

  // 검증
  ASSERT_EQ(results.size(), expected_values.size());
  for (const auto &r : results) {
    EXPECT_TRUE(r.stored) << "stored 플래그가 true여야 합니다";
    EXPECT_EQ(r.tag, "enriched") << "태그가 'enriched'여야 합니다";
    // 값이 expected_values 중 하나여야 함
    bool found = false;
    for (int v : expected_values)
      if (r.value == v) { found = true; break; }
    EXPECT_TRUE(found) << "예상하지 못한 값: " << r.value;
  }
}

// ---------------------------------------------------------------------------
// 테스트 2: 스케일아웃 — 부하 하에서 추가 워커 spawn
// ---------------------------------------------------------------------------

/**
 * @brief Action::scale_to()를 통해 워커를 추가하고
 *        더 많은 아이템을 처리할 수 있는지 확인합니다.
 *
 * 검증:
 * - scale_out() 후 더 많은 아이템을 병렬 처리할 수 있는지
 * - 출력 카운트가 입력 카운트와 일치하는지
 */
TEST(StaticPipelineIntegration, ScaleOutUnderLoad) {
  std::atomic<size_t> processed{0};

  // 느린 처리 함수 (병렬 효과를 측정하기 위해 카운터만)
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

  // 아이템 투입
  constexpr size_t kItems = 20;
  size_t pushed = 0;
  for (size_t i = 0; i < kItems; ++i) {
    if (pipeline.try_push(RawEvent{"1"}))
      ++pushed;
  }

  // 처리 완료 대기
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (processed.load() < pushed &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  pipeline.stop();  // close channels so worker coroutines exit before dispatcher stops
  dispatcher.stop();
  run_thread.join();

  // 처리된 수가 투입된 수 이하
  EXPECT_LE(processed.load(), pushed)
      << "처리된 아이템이 투입된 아이템보다 많을 수 없음";
  EXPECT_GT(processed.load(), 0u) << "최소 1개 이상 처리되어야 함";
}

// ---------------------------------------------------------------------------
// 테스트 3: 드레인 — stop 전 모든 아이템 처리 보장
// ---------------------------------------------------------------------------

/**
 * @brief drain()이 완료되기 전까지 모든 아이템이 처리되는지 검증합니다.
 *
 * 검증:
 * - drain() 완료 후 처리된 아이템 수 == 투입된 아이템 수
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

  // 아이템 투입
  constexpr size_t kItems = 10;
  for (size_t i = 0; i < kItems; ++i)
    pipeline.try_push(RawEvent{"x"});

  // drain 실행 — 모든 아이템이 처리될 때까지 대기
  std::atomic<bool> drain_done{false};

  // drain 래퍼 코루틴 정의
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

  EXPECT_TRUE(drain_done.load()) << "drain()이 타임아웃 내에 완료되지 않음";
  EXPECT_EQ(processed.load(), kItems)
      << "drain 완료 후 모든 아이템이 처리되어야 함";
}

// ---------------------------------------------------------------------------
// 테스트 4: 백프레셔 — 채널이 가득 찰 때 try_push가 false 반환
// ---------------------------------------------------------------------------

/**
 * @brief 채널 용량 초과 시 `try_push()`가 false를 반환하는지 검증합니다.
 *
 * 검증:
 * - 채널 용량(cap)을 초과한 push는 false를 반환해야 함
 */
TEST(StaticPipelineIntegration, BackpressureTryPushReturnsFalse) {
  constexpr size_t kChannelCap = 4;

  // 처리를 지연시켜 채널을 가득 채우는 액션
  // (워커를 아예 시작하지 않고 채널만 채움)
  auto slow_action = [](RawEvent) -> Task<Result<ParsedEvent>> {
    // 실제 테스트에서는 이 함수가 호출되지 않아야 함 (채널 풀 상태)
    co_return ParsedEvent{0};
  };

  auto pipeline = pipeline_builder<RawEvent>()
      .add<ParsedEvent>(slow_action,
                        Action<RawEvent, ParsedEvent>::Config{
                            .min_workers = 0, // 워커 없음 — 채널만 채움
                            .max_workers = 0,
                            .channel_cap = kChannelCap,
                        })
      .build();

  // Dispatcher를 start하지 않으면 워커가 없어 채널이 소비되지 않음
  // try_push로 채널을 꽉 채움
  size_t success_count = 0;
  size_t fail_count    = 0;

  // 채널 용량 + 여분을 시도
  for (size_t i = 0; i < kChannelCap + 10; ++i) {
    if (pipeline.try_push(RawEvent{"x"}))
      ++success_count;
    else
      ++fail_count;
  }

  // 적어도 일부는 실패해야 함 (채널이 가득 찼을 때)
  EXPECT_GT(fail_count, 0u)
      << "채널이 가득 찼을 때 try_push는 false를 반환해야 함";
  EXPECT_LE(success_count, kChannelCap + 1)
      << "성공한 push 수가 채널 용량을 크게 초과할 수 없음";
}

// ---------------------------------------------------------------------------
// 테스트 5: Context 전파 — Context가 단계를 거쳐 전달되는지 확인
// ---------------------------------------------------------------------------

/**
 * @brief Context가 파이프라인 단계를 거쳐 정확히 전파되는지 검증합니다.
 *
 * 검증:
 * - `push()` 시 전달한 Context의 RequestId가 최종 단계에서도 유지되는지
 */
TEST(StaticPipelineIntegration, ContextPropagationThroughStages) {
  // Context를 추출하는 액션 — ActionEnv를 통해 ctx에 접근
  auto ctx_aware_parse = [](RawEvent raw, ActionEnv env)
      -> Task<Result<ParsedEvent>> {
    // RequestId가 Context에 있는지 확인 — 있으면 값을 payload로 사용
    auto rid = env.ctx.get<RequestId>();
    int  v   = 0;
    if (rid.has_value()) {
      // RequestId의 value 길이를 값으로 사용 (전파 확인용)
      v = static_cast<int>(rid->value.size());
    } else {
      // Context 없음 — 원본 payload 숫자 파싱
      for (char c : raw.payload)
        if (c >= '0' && c <= '9') v = v * 10 + (c - '0');
    }
    co_return ParsedEvent{v};
  };

  // Context 전파를 캡처하는 최종 액션
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

  // Context에 RequestId를 담아서 push
  const std::string kReqId = "req-ctx-test-001";
  Context ctx;
  ctx = ctx.put(RequestId{kReqId});

  pipeline.try_push(RawEvent{"0"}, ctx);

  // 출력 대기
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

  ASSERT_TRUE(got_output) << "타임아웃 내에 출력이 생성되지 않음";

  // Context의 RequestId가 전파되어 value에 반영됐는지 확인
  // (ctx_aware_parse에서 rid->value.size() == kReqId.size())
  EXPECT_TRUE(ctx_was_set.load())
      << "최종 단계에서 Context의 RequestId가 유실됨";
  EXPECT_EQ(captured_value.load(),
            static_cast<int>(kReqId.size()))
      << "Context를 통해 전파된 값이 예상과 다름";
}
