/**
 * @file examples/pipeline_hardware_batching.cpp
 * @brief Pipeline Guide §6 Recipe B: Hardware Batching (NPU) example.
 *
 * Scenario:
 *   Incoming camera frames are accumulated by BatchAction (up to 8 at a time)
 *   and then submitted for NPU inference (simulated).
 *   Inference results are forwarded to the next stage (post-processing).
 *
 * Guide excerpt (Recipe B):
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
#include <thread>
#include <vector>
#include <qbuem/compat/print.hpp>

using namespace qbuem;
using namespace std::chrono_literals;
using std::println;

// ─── Domain types ─────────────────────────────────────────────────────────────

struct CameraFrame {
    int id;
    std::vector<uint8_t> pixels; ///< omitted — only size is shown in this example
    size_t width  = 224;
    size_t height = 224;
};

struct InferenceResult {
    int    frame_id;
    float  confidence;
    int    class_id;
};

// ─── NPU inference simulation ─────────────────────────────────────────────────

// BatchAction handler: vector<CameraFrame> → vector<InferenceResult>
static Task<Result<std::vector<InferenceResult>>>
npu_infer(std::vector<CameraFrame> batch, ActionEnv /*env*/) {
    std::vector<InferenceResult> results;
    results.reserve(batch.size());
    for (auto& f : batch) {
        // NPU inference simulation (real hardware: DMA transfer + interrupt wait)
        results.push_back(InferenceResult{
            f.id,
            0.9f - 0.01f * (f.id % 10),  // fake confidence
            f.id % 1000,                   // fake class
        });
    }
    co_return results;
}

// Post-processing: confidence threshold filter
static Task<Result<InferenceResult>>
postprocess(InferenceResult r) {
    if (r.confidence < 0.5f) {
        co_return unexpected(std::make_error_code(std::errc::invalid_argument));
    }
    co_return r;
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main() {
    constexpr size_t kBatchSize = 8;
    constexpr size_t kFrames    = 32; // 4 batches

    // BatchAction: accumulate up to 8 frames then run NPU inference
    BatchAction<CameraFrame, InferenceResult> npu{
        npu_infer,
        BatchAction<CameraFrame, InferenceResult>::Config{
            .max_batch_size = kBatchSize,
            .max_wait_ms    = 10,
            .workers        = 1,
            .channel_cap    = 256,
        }
    };

    // Post-processing Action
    Action<InferenceResult, InferenceResult> post{postprocess};

    // Connect pipeline: npu.output() → post.input()
    Dispatcher dispatcher(2);
    auto final_out = std::make_shared<AsyncChannel<ContextualItem<InferenceResult>>>(256);
    npu.start(dispatcher);
    post.start(dispatcher, final_out);

    // Bridge coroutine: BatchAction output → postprocess input
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

    // Push frames
    std::atomic<size_t> batches_sent{0};
    for (size_t i = 0; i < kFrames; ++i) {
        npu.try_push(CameraFrame{static_cast<int>(i)});
        if ((i + 1) % kBatchSize == 0) ++batches_sent;
    }

    // Collect results
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

    println("[hw-batching] frames={}/{} batches={}",
              received, kFrames, batches_sent.load());

    return (received == kFrames) ? 0 : 1;
}
