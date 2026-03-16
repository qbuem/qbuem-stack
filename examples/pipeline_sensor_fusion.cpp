/**
 * @file examples/pipeline_sensor_fusion.cpp
 * @brief Pipeline Guide §6 Recipe A: Sensor Fusion (N:1 Sync) 예시.
 *
 * 시나리오:
 *   IMU (가속도/자이로) 데이터와 GPS 데이터가 서로 다른 속도로 들어옵니다.
 *   ServiceRegistry에 partial 데이터를 저장하고, 두 센서 데이터가
 *   같은 Context ID로 도착하면 Fusion을 수행합니다.
 *
 * 가이드 원문 (Recipe A):
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
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

using namespace qbuem;
using namespace std::chrono_literals;

// ─── 도메인 타입 ─────────────────────────────────────────────────────────────

struct ImuSample {
    std::string frame_id;
    float ax, ay, az;   ///< 가속도
    float gx, gy, gz;   ///< 자이로
};

struct GpsSample {
    std::string frame_id;
    double lat, lng, alt;
};

// 융합 결과
struct FusedPose {
    std::string frame_id;
    float ax, ay, az;
    double lat, lng, alt;
    bool complete = false;
};

// ServiceRegistry에 저장되는 수집 버퍼
struct FusionBuffer {
    std::mutex                              mu;
    std::unordered_map<std::string, ImuSample> imu;
    std::unordered_map<std::string, GpsSample> gps;
};

// ─── 메시지 래퍼 ─────────────────────────────────────────────────────────────

// 두 센서를 하나의 파이프라인에 입력하기 위한 variant
struct SensorMsg {
    enum class Kind { IMU, GPS } kind;
    ImuSample imu_data{};
    GpsSample gps_data{};
};

// ─── 액션 함수 ───────────────────────────────────────────────────────────────

// Gather 액션: ServiceRegistry에 partial 데이터를 저장하고
// 양쪽 데이터가 모이면 FusedPose를 방출합니다.
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
            // 두 센서 데이터가 모임 → 융합
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

    // 아직 짝이 없으면 빈 결과 반환 (파이프라인에서 필터링 필요)
    co_return FusedPose{fid, 0,0,0, 0,0,0, false};
}

// ─── main ────────────────────────────────────────────────────────────────────

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
    std::thread run_th([&] { dispatcher.run(); });

    // 10 프레임: IMU + GPS 각각 투입 (순서 섞음)
    constexpr size_t kFrames = 10;
    for (size_t i = 0; i < kFrames; ++i) {
        std::string fid = "f" + std::to_string(i);
        // GPS 먼저
        gather.try_push(SensorMsg{
            SensorMsg::Kind::GPS, {},
            GpsSample{fid, 37.5 + i*0.001, 127.0 + i*0.001, 50.0}});
        // IMU 나중
        gather.try_push(SensorMsg{
            SensorMsg::Kind::IMU,
            ImuSample{fid, 0.1f*i, 0.2f*i, 9.8f, 0.f, 0.f, 0.f}});
    }

    // 결과 수집
    size_t fused = 0;
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (fused < kFrames && std::chrono::steady_clock::now() < deadline) {
        auto item = out_ch->try_recv();
        if (item && item->value.complete) {
            ++fused;
            std::cout << "[fusion] frame=" << item->value.frame_id
                      << " lat=" << item->value.lat << "\n";
        } else {
            std::this_thread::sleep_for(1ms);
        }
    }

    gather.stop();
    dispatcher.stop();
    run_th.join();

    std::cout << "[sensor-fusion] fused=" << fused << "/" << kFrames << "\n";
    return (fused == kFrames) ? 0 : 1;
}
