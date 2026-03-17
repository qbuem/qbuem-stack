/**
 * @file examples/pipeline_hardware_batching.cpp
 * @brief Pipeline Guide §6 Recipe B: Hardware Batching (NPU) 예시.
 *
 * 시나리오:
 *   카메라 프레임이 들어오면 BatchAction이 최대 8개씩 모아
 *   NPU 추론(시뮬레이션)을 수행합니다.
 *   추론 결과는 다음 스테이지(후처리)로 전달됩니다.
 *
 * 가이드 원문 (Recipe B):
 *   1. Action uses WorkerLocal storage to accumulate 8 frames.
 *   2. On the 8th frame, emit the batch and clear the buffer.
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/action.hpp>
#include <qbuem/pipeline/batch_action.hpp>
#include <qbuem/pipeline/static_pipeline.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

using namespace qbuem;
using namespace std::chrono_literals;

// ─── 도메인 타입 ─────────────────────────────────────────────────────────────

struct CameraFrame {
    int id;
    std::vector<uint8_t> pixels; ///< 생략 — 예시에서는 크기만 표시
    size_t width  = 224;
    size_t height = 224;
};

struct InferenceResult {
    int    frame_id;
    float  confidence;
    int    class_id;
};

// ─── NPU 추론 시뮬레이션 ─────────────────────────────────────────────────────

// BatchAction 처리 함수: vector<CameraFrame> → vector<InferenceResult>
static Task<Result<std::vector<InferenceResult>>>
npu_infer(std::vector<CameraFrame> batch, ActionEnv /*env*/) {
    std::vector<InferenceResult> results;
    results.reserve(batch.size());
    for (auto& f : batch) {
        // NPU 추론 시뮬레이션 (실제 환경에서는 DMA 전송 + 인터럽트 대기)
        results.push_back(InferenceResult{
            f.id,
            0.9f - 0.01f * (f.id % 10),  // fake confidence
            f.id % 1000,                   // fake class
        });
    }
    co_return results;
}

// 후처리: confidence 임계값 필터
static Task<Result<InferenceResult>>
postprocess(InferenceResult r) {
    if (r.confidence < 0.5f) {
        co_return unexpected(std::make_error_code(std::errc::invalid_argument));
    }
    co_return r;
}

// ─── main ────────────────────────────────────────────────────────────────────

int main() {
    constexpr size_t kBatchSize = 8;
    constexpr size_t kFrames    = 32; // 4 batches

    // BatchAction: 최대 8프레임 누적 후 NPU 추론
    BatchAction<CameraFrame, InferenceResult> npu{
        npu_infer,
        BatchAction<CameraFrame, InferenceResult>::Config{
            .max_batch_size = kBatchSize,
            .max_wait_ms    = 10,
            .workers        = 1,
            .channel_cap    = 256,
        }
    };

    // 후처리 Action
    Action<InferenceResult, InferenceResult> post{postprocess};

    // 파이프라인 연결: npu.output() → post.input()
    Dispatcher dispatcher(2);
    auto final_out = std::make_shared<AsyncChannel<ContextualItem<InferenceResult>>>(256);
    npu.start(dispatcher);
    post.start(dispatcher, final_out);

    // BatchAction 출력 → postprocess 입력 브릿지 코루틴
    auto bridge = [&]() -> Task<void> {
        auto src = npu.output();
        while (true) {
            auto item = co_await src->recv();
            if (!item) co_return;
            co_await post.push(item->value, item->ctx);
        }
    };
    dispatcher.spawn(bridge());

    std::jthread run_th([&] { dispatcher.run(); });

    // 프레임 투입
    std::atomic<size_t> batches_sent{0};
    for (size_t i = 0; i < kFrames; ++i) {
        npu.try_push(CameraFrame{static_cast<int>(i)});
        if ((i + 1) % kBatchSize == 0) ++batches_sent;
    }

    // 결과 수집
    size_t received = 0;
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (received < kFrames && std::chrono::steady_clock::now() < deadline) {
        auto r = final_out->try_recv();
        if (r) {
            ++received;
        } else {
            std::this_thread::sleep_for(1ms);
        }
    }

    npu.stop();
    post.stop();
    dispatcher.stop();
    run_th.join();

    std::cout << "[hw-batching] frames=" << received
              << "/" << kFrames
              << " batches=" << batches_sent.load() << "\n";

    return (received == kFrames) ? 0 : 1;
}
