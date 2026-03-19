/**
 * @file sensor_fusion_example.cpp
 * @brief Hardware sensor fusion example — IMU + GPS + LiDAR → StaticPipeline fused state output.
 *
 * ## Overview
 * Generates mock sensor data without real hardware to demonstrate a multi-sensor fusion system
 * using qbuem StaticPipeline + DynamicPipeline.
 *
 * ## Architecture
 * ```
 * [MockIMU]  ─┐
 * [MockGPS]  ─┼─→ RawSensorFrame
 * [MockLiDAR]─┘         │
 *                        ▼
 *              StaticPipeline<RawSensorFrame, FusedState>
 *                ├─ Stage1: validate_frame()   — validation
 *                ├─ Stage2: preprocess_frame() — unit conversion + LPF noise filter
 *                ├─ Stage3: fuse_sensors()     — GPS + IMU + LiDAR weighted fusion
 *                ├─ Stage4: smooth_state()     — moving average smoothing
 *                └─ (sink) FusionSink::sink()  — logging + statistics
 *
 *              DynamicPipeline<FusedState>  (separate runtime hot-swap demo)
 *                ├─ "alert_check" — confidence threshold alert
 *                └─ runtime hot-swap → replace with "alert_strict"
 * ```
 *
 * ## Coverage
 * - StaticPipeline<In, Out> + PipelineBuilder + with_sink()
 * - DynamicPipeline<T> + add_stage() + replace_stage()
 * - Dispatcher + Task<T>
 * - All sensors use mock implementations (no real hardware required)
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/dynamic_pipeline.hpp>
#include <qbuem/pipeline/static_pipeline.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <qbuem/compat/print.hpp>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace qbuem;
using namespace std::chrono_literals;

// ─────────────────────────────────────────────────────────────────────────────
// §0  Sensor data type definitions
// ─────────────────────────────────────────────────────────────────────────────

struct Vec3 { double x{0}, y{0}, z{0}; };
struct Quaternion { double w{1}, x{0}, y{0}, z{0}; };

struct ImuReading {
    Vec3   accel{0, 0, 9.81};  ///< acceleration (m/s²)
    Vec3   gyro{};              ///< angular velocity (rad/s)
    double temperature{25};
    uint64_t timestamp_us{0};
};

struct GpsReading {
    double latitude{37.5665};
    double longitude{126.9780};
    double altitude{50.0};
    double hdop{1.2};
    double vdop{1.8};
    uint64_t timestamp_us{0};
};

struct LidarReading {
    double nearest_obstacle_m{10.0};
    Vec3   floor_normal{0, 0, 1};
    int    point_count{1024};
    uint64_t timestamp_us{0};
};

struct RawSensorFrame {
    ImuReading   imu;
    GpsReading   gps;
    LidarReading lidar;
    uint64_t     frame_id{0};
};

struct FusedState {
    Vec3       position;    ///< ENU coordinates (m)
    Vec3       velocity;    ///< velocity vector (m/s)
    Quaternion orientation;
    double     confidence{0};
    uint64_t   frame_id{0};
};

// ─────────────────────────────────────────────────────────────────────────────
// §1  Mock sensors — simulation data without real hardware
// ─────────────────────────────────────────────────────────────────────────────

struct MockIMU {
    ImuReading read(uint64_t ts_us) const {
        double t = static_cast<double>(ts_us) * 1e-6;
        return {
            .accel       = {1.0 + 0.02*std::sin(t*3.0), 0.01*std::cos(t*5.0), 9.81 + 0.01*std::sin(t*7.0)},
            .gyro        = {0.001*std::cos(t), 0.002*std::sin(t*2), 0.005*std::cos(t*1.5)},
            .temperature = 25.0 + 0.5*std::sin(t*0.1),
            .timestamp_us = ts_us,
        };
    }
};

struct MockGPS {
    GpsReading read(uint64_t ts_us) const {
        double t = static_cast<double>(ts_us) * 1e-6;
        return {
            .latitude  = 37.5665 + 0.0001*t,
            .longitude = 126.9780 + 0.00005*t,
            .altitude  = 50.0 + 0.1*std::sin(t*0.5),
            .hdop      = 1.2 + 0.1*std::sin(t),
            .vdop      = 1.8 + 0.1*std::cos(t),
            .timestamp_us = ts_us,
        };
    }
};

struct MockLiDAR {
    LidarReading read(uint64_t ts_us) const {
        double t = static_cast<double>(ts_us) * 1e-6;
        return {
            .nearest_obstacle_m = 5.0 + 2.0*std::abs(std::sin(t*0.3)),
            .floor_normal       = {0.01*std::sin(t*0.2), 0.01*std::cos(t*0.3), 1.0},
            .point_count        = 1024 + static_cast<int>(100*std::sin(t)),
            .timestamp_us       = ts_us,
        };
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// §2  StaticPipeline stage functions
// ─────────────────────────────────────────────────────────────────────────────

// Stage 1: validation
static Task<Result<RawSensorFrame>> validate_frame(RawSensorFrame f, ActionEnv) {
    if (f.gps.latitude < -90.0 || f.gps.latitude > 90.0)
        co_return unexpected(std::make_error_code(std::errc::invalid_argument));
    constexpr double kMaxAccel = 50.0 * 9.81;
    if (std::abs(f.imu.accel.z) > kMaxAccel)
        co_return unexpected(std::make_error_code(std::errc::invalid_argument));
    co_return f;
}

// Stage 2: preprocessing — temperature correction + low-pass filter (α=0.1)
static Vec3 g_accel_lpf{0, 0, 9.81};
static Task<Result<RawSensorFrame>> preprocess_frame(RawSensorFrame f, ActionEnv) {
    double tf = 1.0 + 0.0001 * (f.imu.temperature - 25.0);
    f.imu.accel.x *= tf;
    f.imu.accel.y *= tf;
    f.imu.accel.z *= tf;
    constexpr double kA = 0.1;
    g_accel_lpf.x += kA * (f.imu.accel.x - g_accel_lpf.x);
    g_accel_lpf.y += kA * (f.imu.accel.y - g_accel_lpf.y);
    g_accel_lpf.z += kA * (f.imu.accel.z - g_accel_lpf.z);
    f.imu.accel = g_accel_lpf;
    co_return f;
}

// Stage 3: GPS + IMU + LiDAR weighted fusion
static Task<Result<FusedState>> fuse_sensors(RawSensorFrame f, ActionEnv) {
    FusedState s;
    s.frame_id = f.frame_id;
    // GPS → ENU (origin 37.5665°N, 126.9780°E)
    constexpr double kLat0 = 37.5665, kLon0 = 126.9780, kMpD = 111320.0;
    s.position.x = (f.gps.longitude - kLon0) * kMpD * std::cos(kLat0 * M_PI / 180.0);
    s.position.y = (f.gps.latitude  - kLat0) * kMpD;
    s.position.z = f.gps.altitude;
    // IMU velocity integration (dt = 20ms)
    s.velocity.x = f.imu.accel.x * 0.02;
    s.velocity.y = f.imu.accel.y * 0.02;
    s.velocity.z = 0.0;
    // LiDAR floor normal → roll/pitch → quaternion
    double roll  = std::atan2(f.lidar.floor_normal.x, f.lidar.floor_normal.z);
    double pitch = std::atan2(f.lidar.floor_normal.y, f.lidar.floor_normal.z);
    s.orientation.w = std::cos(roll/2)*std::cos(pitch/2);
    s.orientation.x = std::sin(roll/2)*std::cos(pitch/2);
    s.orientation.y = std::cos(roll/2)*std::sin(pitch/2);
    s.orientation.z = 0.0;
    // confidence = GPS quality * 0.6 + LiDAR density * 0.4
    double gps_q   = std::max(0.0, 1.0 - (f.gps.hdop - 1.0) / 10.0);
    double lidar_q = std::min(1.0, f.lidar.point_count / 2048.0);
    s.confidence   = 0.6 * gps_q + 0.4 * lidar_q;
    co_return s;
}

// Stage 4: moving average smoothing (window size 5)
static std::array<FusedState, 5> g_history{};
static size_t g_hist_idx{0}, g_hist_n{0};

static Task<Result<FusedState>> smooth_state(FusedState s, ActionEnv) {
    g_history[g_hist_idx % g_history.size()] = s;
    ++g_hist_idx;
    g_hist_n = std::min(g_hist_n + 1, g_history.size());
    Vec3 avg{};
    for (size_t i = 0; i < g_hist_n; ++i) {
        avg.x += g_history[i].position.x;
        avg.y += g_history[i].position.y;
        avg.z += g_history[i].position.z;
    }
    double n = static_cast<double>(g_hist_n);
    s.position = {avg.x/n, avg.y/n, avg.z/n};
    co_return s;
}

// ─────────────────────────────────────────────────────────────────────────────
// §3  Sink — StaticPipeline final output collector
// ─────────────────────────────────────────────────────────────────────────────

static std::atomic<int> g_sink_count{0};

struct FusionSink {
    Result<void> init() {
        std::println("  [Sink] initialized");
        return {};
    }
    Task<Result<void>> sink(const FusedState& s) {
        std::println(
            "  [Fused#{:3}] pos=({:.2f}, {:.2f}, {:.2f})m  "
            "vel=({:.3f}, {:.3f})m/s  conf={:.2f}",
            s.frame_id,
            s.position.x, s.position.y, s.position.z,
            s.velocity.x, s.velocity.y, s.confidence);
        g_sink_count.fetch_add(1, std::memory_order_relaxed);
        co_return Result<void>{};
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// §4  DynamicPipeline stage — confidence alert (runtime hot-swap demo)
// ─────────────────────────────────────────────────────────────────────────────

static std::atomic<int> g_alert_count{0};

// initial stage: alert when confidence < 0.5
static Task<Result<FusedState>> alert_loose(FusedState s, ActionEnv) {
    if (s.confidence < 0.5) {
        std::println("  [ALERT-LOOSE] Low confidence: {:.2f} (frame #{:})",
                    s.confidence, s.frame_id);
        g_alert_count.fetch_add(1, std::memory_order_relaxed);
    }
    co_return s;
}

// hot-swap stage: alert when confidence < 0.7
static Task<Result<FusedState>> alert_strict(FusedState s, ActionEnv) {
    if (s.confidence < 0.7) {
        std::println("  [ALERT-STRICT] Low confidence: {:.2f} (frame #{:})",
                    s.confidence, s.frame_id);
        g_alert_count.fetch_add(1, std::memory_order_relaxed);
    }
    co_return s;
}

// ─────────────────────────────────────────────────────────────────────────────
// §5  Main fusion loop coroutine
// ─────────────────────────────────────────────────────────────────────────────

static std::atomic<bool> g_fusion_done{false};
static std::atomic<bool> g_dynamic_done{false};

static Task<void> run_fusion_pipeline(
    std::shared_ptr<StaticPipeline<RawSensorFrame, FusedState>> pipe,
    int count)
{
    MockIMU   imu;
    MockGPS   gps;
    MockLiDAR lidar;

    for (int i = 0; i < count; ++i) {
        uint64_t ts = static_cast<uint64_t>(i) * 20000ULL; // 20ms
        co_await pipe->push(RawSensorFrame{
            .imu     = imu.read(ts),
            .gps     = gps.read(ts),
            .lidar   = lidar.read(ts),
            .frame_id = static_cast<uint64_t>(i + 1),
        });
    }
    g_fusion_done.store(true, std::memory_order_release);
    co_return;
}

static Task<void> run_dynamic_demo(
    std::shared_ptr<DynamicPipeline<FusedState>> dp,
    int count)
{
    MockIMU   imu;
    MockGPS   gps;
    MockLiDAR lidar;

    for (int i = 0; i < count; ++i) {
        uint64_t ts = static_cast<uint64_t>(i) * 20000ULL;
        // directly create FusedState and push to DynamicPipeline
        auto imu_r = imu.read(ts);
        auto gps_r = gps.read(ts);
        auto ldr_r = lidar.read(ts);
        double gps_q   = std::max(0.0, 1.0 - (gps_r.hdop - 1.0) / 10.0);
        double lidar_q = std::min(1.0, ldr_r.point_count / 2048.0);
        FusedState s;
        s.frame_id   = static_cast<uint64_t>(i + 1);
        s.confidence = 0.6*gps_q + 0.4*lidar_q;
        s.position   = {(double)i * 0.1, 0.0, 50.0};
        dp->try_push(s);

        // hot-swap alert threshold at midpoint
        if (i == count / 2) {
            std::println("  [hot-swap] 'alert_check' → 'alert_strict' replacement");
            dp->hot_swap("alert_check", alert_strict);
        }
    }
    g_dynamic_done.store(true, std::memory_order_release);
    co_return;
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::println("=== qbuem Hardware Sensor Fusion Example ===\n");
    std::println("Note: Uses Mock IMU/GPS/LiDAR data (no real hardware required).\n");

    constexpr int kFrames = 10;

    Dispatcher disp(2);
    std::jthread worker([&] { disp.run(); });

    // ── §A  StaticPipeline: validate → preprocess → fuse → smooth → sink ──
    std::println("── §A  StaticPipeline setup ──");
    auto static_pipe = std::make_shared<StaticPipeline<RawSensorFrame, FusedState>>(
        PipelineBuilder<RawSensorFrame>{}
            .add<RawSensorFrame>(validate_frame)
            .add<RawSensorFrame>(preprocess_frame)
            .add<FusedState>(fuse_sensors)
            .add<FusedState>(smooth_state)
            .with_sink(FusionSink{})
            .build());
    static_pipe->start(disp);

    std::println("\n── §B  StaticPipeline sensor fusion loop ({} frames) ──", kFrames);
    disp.spawn(run_fusion_pipeline(static_pipe, kFrames));

    // wait for completion
    {
        auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!g_fusion_done.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(10ms);
        std::this_thread::sleep_for(200ms); // wait for sink processing to complete
    }
    std::println("  Processing complete: {} / {} frames", g_sink_count.load(), kFrames);

    // ── §C  DynamicPipeline: alert_check + runtime hot-swap ──
    std::println("\n── §C  DynamicPipeline alert system (runtime hot-swap demo) ──");
    auto dp = std::make_shared<DynamicPipeline<FusedState>>();
    dp->add_stage("alert_check", alert_loose);
    dp->start(disp);

    disp.spawn(run_dynamic_demo(dp, kFrames));
    {
        auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!g_dynamic_done.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(10ms);
        std::this_thread::sleep_for(200ms);
    }

    disp.stop();
    worker.join();

    std::println("\n── Results Summary ──");
    std::println("  StaticPipeline frames processed: {} / {}", g_sink_count.load(), kFrames);
    std::println("  DynamicPipeline alerts triggered: {} events", g_alert_count.load());
    std::println("  Sensor fusion stages: validate → preprocess(LPF) → fuse → smooth");
    std::println("  DynamicPipeline: alert_loose → hot-swap → alert_strict");

    if (g_sink_count.load() > 0)
        std::println("\nsensor_fusion_example: ALL OK");
    else
        std::println("\nsensor_fusion_example: WARN — no output");

    return 0;
}
