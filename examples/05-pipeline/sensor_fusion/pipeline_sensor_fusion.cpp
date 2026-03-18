/**
 * @file examples/pipeline_sensor_fusion.cpp
 * @brief Pipeline Guide §6 Recipe A: Sensor Fusion (N:1 Sync) example.
 *
 * Scenario:
 *   IMU (accelerometer/gyro) data and GPS data arrive at different rates.
 *   Partial data is stored in the ServiceRegistry, and when both sensor
 *   readings arrive with the same Context ID, fusion is performed.
 *
 * Guide excerpt (Recipe A):
 *   1. Use a Gather Action that stores partial data in the ServiceRegistry.
 *   2. Once all parts arrive (aligned by Context ID), compute the fusion and
 *      emit the result.
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/action.hpp>
#include <qbuem/pipeline/action_env.hpp>
#include <qbuem/pipeline/async_channel.hpp>
#include <qbuem/pipeline/context.hpp>
#include <qbuem/pipeline/service_registry.hpp>
#include <qbuem/pipeline/static_pipeline.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <qbuem/compat/print.hpp>

using namespace qbuem;
using namespace std::chrono_literals;
using std::println;

// ─── Domain types ─────────────────────────────────────────────────────────────

struct ImuSample {
    std::string frame_id;
    float ax, ay, az;   ///< accelerometer
    float gx, gy, gz;   ///< gyroscope
};

struct GpsSample {
    std::string frame_id;
    double lat, lng, alt;
};

// Fusion result
struct FusedPose {
    std::string frame_id;
    float ax, ay, az;
    double lat, lng, alt;
    bool complete = false;
};

// Accumulation buffer stored in ServiceRegistry
struct FusionBuffer {
    std::mutex                              mu;
    std::unordered_map<std::string, ImuSample> imu;
    std::unordered_map<std::string, GpsSample> gps;
};

// ─── Message wrapper ──────────────────────────────────────────────────────────

// Variant to feed both sensor types into a single pipeline
struct SensorMsg {
    enum class Kind { IMU, GPS } kind;
    ImuSample imu_data{};
    GpsSample gps_data{};
};

// ─── Action functions ─────────────────────────────────────────────────────────

// Gather action: store partial data in ServiceRegistry and emit FusedPose
// once both readings for the same frame ID are available.
static Task<Result<FusedPose>> gather_fuse(SensorMsg msg, ActionEnv env) {
    auto& buf = env.registry->get_or_create<FusionBuffer>();
    std::string fid;

    {
        std::lock_guard lk(buf.mu);
        if (msg.kind == SensorMsg::Kind::IMU) {
            fid = msg.imu_data.frame_id;
            buf.imu[fid] = msg.imu_data;
        } else {
            fid = msg.gps_data.frame_id;
            buf.gps[fid] = msg.gps_data;
        }

        auto iit = buf.imu.find(fid);
        auto git = buf.gps.find(fid);
        if (iit != buf.imu.end() && git != buf.gps.end()) {
            // Both sensor readings available → fuse
            FusedPose pose{
                fid,
                iit->second.ax, iit->second.ay, iit->second.az,
                git->second.lat, git->second.lng, git->second.alt,
                true,
            };
            buf.imu.erase(iit);
            buf.gps.erase(git);
            co_return pose;
        }
    }

    // No matching pair yet — return empty result (pipeline must filter these out)
    co_return FusedPose{fid, 0,0,0, 0,0,0, false};
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main() {
    ServiceRegistry registry;

    Action<SensorMsg, FusedPose>::Config cfg;
    cfg.min_workers = 1;
    cfg.max_workers = 2;
    cfg.channel_cap = 128;
    cfg.registry    = &registry;

    Action<SensorMsg, FusedPose> gather(gather_fuse, cfg);

    Dispatcher dispatcher(1);
    auto out_ch = std::make_shared<AsyncChannel<ContextualItem<FusedPose>>>(128);
    gather.start(dispatcher, out_ch);
    std::jthread run_th([&] { dispatcher.run(); });

    // 10 frames: push IMU + GPS each (interleaved order)
    constexpr size_t kFrames = 10;
    for (size_t i = 0; i < kFrames; ++i) {
        std::string fid = "f" + std::to_string(i);
        // GPS first
        gather.try_push(SensorMsg{
            SensorMsg::Kind::GPS, {},
            GpsSample{fid, 37.5 + i*0.001, 127.0 + i*0.001, 50.0}});
        // IMU second
        gather.try_push(SensorMsg{
            SensorMsg::Kind::IMU,
            ImuSample{fid, 0.1f*i, 0.2f*i, 9.8f, 0.f, 0.f, 0.f}});
    }

    // Collect results
    size_t fused = 0;
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (fused < kFrames && std::chrono::steady_clock::now() < deadline) {
        auto item = out_ch->try_recv();
        if (item && item->value.complete) {
            ++fused;
            println("[fusion] frame={} lat={}", item->value.frame_id, item->value.lat);
        } else {
            std::this_thread::sleep_for(1ms);
        }
    }

    gather.stop();
    dispatcher.stop();
    run_th.join();

    println("[sensor-fusion] fused={}/{}", fused, kFrames);
    return (fused == kFrames) ? 0 : 1;
}
