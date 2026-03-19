/**
 * @file autonomous_driving_fusion.cpp
 * @brief Autonomous driving multi-sensor fusion example
 *
 * ## Overview
 * Simulates 6 sensor types via a Mock HAL layer without real hardware.
 * Implements a Perception → Planning → Control pipeline using
 * qbuem StaticPipeline + DynamicPipeline + MessageBus.
 *
 * ## Architecture
 * ```
 * ┌─────────────────────────── Mock HAL Layer ──────────────────────────────┐
 * │  MockCamera  MockRadar  MockLiDAR  MockIMU  MockGPS  MockUltrasonic     │
 * └─────────────────────┬───────────────────────────────────────────────────┘
 *                        │ RawSensorBundle
 *                        ▼
 *          ┌─────────────────────────────────────────┐
 *          │   StaticPipeline<RawSensorBundle,        │
 *          │                  PerceptionResult>        │
 *          │  1. validate_bundle()  — sensor timestamp alignment  │
 *          │  2. calibrate()        — extrinsic calibration       │
 *          │  3. fuse_perception()  — EKF-style weighted fusion   │
 *          │  4. track_objects()    — obstacle tracking + SORT    │
 *          └─────────────────┬───────────────────────┘
 *                             │ PerceptionResult
 *                             ▼
 *          ┌─────────────────────────────────────────┐
 *          │   DynamicPipeline<PerceptionResult>      │
 *          │  "obstacle_check" — collision risk calc  │
 *          │  "path_plan"      — target path gen      │
 *          │  "velocity_cmd"   — speed/steer command  │
 *          │  "emergency"      — emergency braking (hotswap) │
 *          └─────────────────┬───────────────────────┘
 *                             │
 *                    MessageBus("vehicle_state")
 *                    MessageBus("alerts")
 *
 * ## Vehicle state machine
 *   IDLE → PERCEPTION → PLANNING → CONTROL → EMERGENCY(hot-swap)
 *
 * ## Coverage
 * - Mock HAL (Camera, Radar, LiDAR, IMU, GPS, Ultrasonic)
 * - StaticPipeline<In, Out> + PipelineBuilder + with_sink()
 * - DynamicPipeline<T> + hot_swap() (normal → emergency)
 * - MessageBus: publish / subscribe
 * - Dispatcher + Task<T> + coroutine lifetime management
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/dynamic_pipeline.hpp>
#include <qbuem/pipeline/message_bus.hpp>
#include <qbuem/pipeline/static_pipeline.hpp>
#include <qbuem/compat/print.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace qbuem;
using namespace std::chrono_literals;

// ─────────────────────────────────────────────────────────────────────────────
// §0  Basic math types
// ─────────────────────────────────────────────────────────────────────────────

struct Vec2 { double x{0}, y{0}; };
struct Vec3 { double x{0}, y{0}, z{0}; };

inline double vec3_len(Vec3 v) {
    return std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
}

// ─────────────────────────────────────────────────────────────────────────────
// §1  Sensor data types (HAL output structs)
// ─────────────────────────────────────────────────────────────────────────────

struct CameraFrame {
    uint32_t width{1920}, height{1080};
    uint8_t  lane_detected{1};
    double   lane_offset_m{0.0};    ///< lateral offset from lane center
    double   forward_clearance_m{50.0};
    uint64_t ts_us{0};
};

struct RadarTrack {
    double   range_m{30.0};
    double   velocity_mps{0.0};    ///< relative velocity (positive = approaching)
    double   azimuth_deg{0.0};
    uint64_t ts_us{0};
};

struct LidarCloud {
    uint32_t point_count{32768};
    double   nearest_m{8.0};       ///< nearest forward point
    Vec3     centroid{0, 0, 1.5};  ///< cluster centroid
    uint64_t ts_us{0};
};

struct ImuSample {
    Vec3     accel{0, 0, 9.81};    ///< m/s²
    Vec3     gyro{};               ///< rad/s
    double   heading_deg{0.0};     ///< absolute bearing
    uint64_t ts_us{0};
};

struct GnssPosition {
    double   lat{37.5665};
    double   lon{126.9780};
    double   alt{50.0};
    double   hdop{1.2};
    uint64_t ts_us{0};
};

struct UltrasonicPing {
    double   front_m{3.5};        ///< ultrasonic front distance
    double   rear_m{5.0};
    double   left_m{1.8};
    double   right_m{1.8};
    uint64_t ts_us{0};
};

// Fusion pipeline input — all sensors bundled per cycle
struct RawSensorBundle {
    CameraFrame    camera;
    RadarTrack     radar;
    LidarCloud     lidar;
    ImuSample      imu;
    GnssPosition   gnss;
    UltrasonicPing ultrasonic;
    uint64_t       cycle_id{0};
    uint64_t       ts_us{0};
};

// ─────────────────────────────────────────────────────────────────────────────
// §2  Perception result types
// ─────────────────────────────────────────────────────────────────────────────

struct TrackedObject {
    uint32_t id{0};
    Vec3     position_enu;  ///< East-North-Up coordinates (m)
    Vec3     velocity;      ///< m/s
    double   ttc_sec{99};   ///< Time-To-Collision
    bool     critical{false};
};

struct PerceptionResult {
    Vec3     ego_pos_enu;          ///< ego vehicle position
    Vec3     ego_vel;              ///< ego vehicle velocity
    double   ego_heading_deg{0};
    double   lane_offset_m{0};
    std::vector<TrackedObject> objects;
    double   confidence{0};
    bool     emergency_flag{false};
    uint64_t cycle_id{0};
};

// ─────────────────────────────────────────────────────────────────────────────
// §3  Mock HAL — generates physics simulation data without real hardware
// ─────────────────────────────────────────────────────────────────────────────

struct MockCamera {
    CameraFrame read(uint64_t ts_us, int cycle) const {
        double t = ts_us * 1e-6;
        // Lane departure: gradually departs during cycles 12-18
        double offset = (cycle >= 12 && cycle <= 18)
            ? 0.15 * std::sin((cycle - 12) * 0.5)
            : 0.02 * std::sin(t * 0.7);
        return {
            .lane_detected      = 1,
            .lane_offset_m      = offset,
            .forward_clearance_m = 40.0 + 15.0 * std::sin(t * 0.2),
            .ts_us              = ts_us,
        };
    }
};

struct MockRadar {
    RadarTrack read(uint64_t ts_us, int cycle) const {
        double t = ts_us * 1e-6;
        // cycles 20-25: lead vehicle suddenly decelerates (closing speed spikes)
        double rel_vel = (cycle >= 20 && cycle <= 25)
            ? 8.0 + 2.0 * (cycle - 20)   // max 18 m/s closing speed
            : 1.5 + 1.0 * std::sin(t * 0.5);
        double range = std::max(4.0, 25.0 - rel_vel * (cycle >= 20 ? (cycle-20)*0.1 : 0.0));
        return {
            .range_m      = range,
            .velocity_mps = rel_vel,
            .azimuth_deg  = 2.0 * std::sin(t * 0.3),
            .ts_us        = ts_us,
        };
    }
};

struct MockLiDAR {
    LidarCloud read(uint64_t ts_us, int cycle) const {
        double t = ts_us * 1e-6;
        double nearest = std::max(3.0, 20.0 - (cycle >= 20 ? (cycle-20)*0.8 : 0.0));
        return {
            .point_count = 32768,
            .nearest_m   = nearest,
            .centroid    = {nearest * std::sin(0.05), 0.5, 1.5},
            .ts_us       = ts_us,
        };
    }
};

struct MockIMU {
    ImuSample read(uint64_t ts_us, int cycle) const {
        double t = ts_us * 1e-6;
        double yaw_rate = (cycle >= 12 && cycle <= 18)
            ? 0.05 * std::sin((cycle-12) * 0.4)
            : 0.005 * std::sin(t * 0.3);
        return {
            .accel   = {0.2 * std::sin(t*0.5), 0.05 * std::cos(t*1.3), 9.81},
            .gyro    = {0.001, yaw_rate, 0.002},
            .heading_deg = std::fmod(15.0 + t * 2.0, 360.0),
            .ts_us   = ts_us,
        };
    }
};

struct MockGNSS {
    GnssPosition read(uint64_t ts_us, int /*cycle*/) const {
        double t = ts_us * 1e-6;
        return {
            .lat  = 37.5665 + t * 2e-5,
            .lon  = 126.9780 + t * 1e-5,
            .alt  = 50.0 + 0.3 * std::sin(t * 0.1),
            .hdop = 1.1 + 0.15 * std::sin(t * 0.5),
            .ts_us = ts_us,
        };
    }
};

struct MockUltrasonic {
    UltrasonicPing read(uint64_t ts_us, int cycle) const {
        double t = ts_us * 1e-6;
        double front = std::max(0.5, 3.5 - (cycle >= 20 ? (cycle-20)*0.15 : 0.0));
        return {
            .front_m = front,
            .rear_m  = 5.0,
            .left_m  = 1.5 + 0.3 * std::sin(t * 0.8),
            .right_m = 1.8 + 0.2 * std::cos(t * 0.6),
            .ts_us   = ts_us,
        };
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// §4  EKF-style state estimator (lightweight version)
// ─────────────────────────────────────────────────────────────────────────────

struct EKFState {
    Vec3   pos{};
    Vec3   vel{};
    double heading{0};
    double pos_var{1.0};  ///< position variance (inverse confidence)
    double vel_var{0.5};
};

// g_ekf_mtx protects all EKF state access.
// fuse_perception releases the mutex before co_await so the
// mutex does not live on the coroutine frame.
static std::mutex  g_ekf_mtx;
static EKFState    g_ekf{};

// ── internal helpers without mutex (must always be called while holding g_ekf_mtx) ──

static void ekf_predict_nolock(double dt, const ImuSample& imu) {
    // Integrate velocity from acceleration
    g_ekf.vel.x += imu.accel.x * dt;
    g_ekf.vel.y += imu.accel.y * dt;
    // Integrate position
    g_ekf.pos.x += g_ekf.vel.x * dt;
    g_ekf.pos.y += g_ekf.vel.y * dt;
    g_ekf.heading = imu.heading_deg;
    // Prediction variance grows
    g_ekf.pos_var += 0.1 * dt;
    g_ekf.vel_var += 0.05 * dt;
}

static void ekf_update_gnss_nolock(const GnssPosition& gnss) {
    constexpr double kMpD = 111320.0;
    constexpr double kLat0 = 37.5665, kLon0 = 126.9780;
    double obs_x = (gnss.lon - kLon0) * kMpD * std::cos(kLat0 * M_PI / 180.0);
    double obs_y = (gnss.lat - kLat0) * kMpD;
    double R = gnss.hdop * 2.0;  // GNSS noise covariance
    double K = g_ekf.pos_var / (g_ekf.pos_var + R);  // Kalman gain
    g_ekf.pos.x += K * (obs_x - g_ekf.pos.x);
    g_ekf.pos.y += K * (obs_y - g_ekf.pos.y);
    g_ekf.pos.z  = gnss.alt;
    g_ekf.pos_var *= (1.0 - K);  // variance decreases after update
}

// ─────────────────────────────────────────────────────────────────────────────
// §5  StaticPipeline stage functions
// ─────────────────────────────────────────────────────────────────────────────

// Stage 1: timestamp alignment check
static Task<Result<RawSensorBundle>> validate_bundle(RawSensorBundle b, ActionEnv) {
    constexpr uint64_t kMaxJitter_us = 50000;  // 50ms
    uint64_t ref = b.ts_us;
    if (std::abs((int64_t)(b.camera.ts_us    - ref)) > (int64_t)kMaxJitter_us ||
        std::abs((int64_t)(b.radar.ts_us     - ref)) > (int64_t)kMaxJitter_us ||
        std::abs((int64_t)(b.lidar.ts_us     - ref)) > (int64_t)kMaxJitter_us) {
        // Timestamp mismatch — normalize
        b.camera.ts_us     = ref;
        b.radar.ts_us      = ref;
        b.lidar.ts_us      = ref;
        b.imu.ts_us        = ref;
        b.gnss.ts_us       = ref;
        b.ultrasonic.ts_us = ref;
    }
    co_return b;
}

// Stage 2: extrinsic calibration (camera distortion, radar bias removal)
static Task<Result<RawSensorBundle>> calibrate(RawSensorBundle b, ActionEnv) {
    // Camera: lens distortion correction (offset bias -0.01m)
    b.camera.lane_offset_m -= 0.01;
    // Radar: range bias correction (+0.3m)
    b.radar.range_m += 0.3;
    // IMU: remove gravity component (subtract 9.81 from z-axis)
    b.imu.accel.z -= 9.81;
    co_return b;
}

// Stage 3: EKF fusion — GNSS + IMU state estimation
static Task<Result<PerceptionResult>> fuse_perception(RawSensorBundle b, ActionEnv) {
    constexpr double kDt = 0.05;  // 50ms cycle

    // Mutex-protected section: perform predict + update + snapshot atomically.
    // Release lock before co_await to keep mutex off the coroutine frame.
    EKFState snap;
    {
        std::lock_guard<std::mutex> lk(g_ekf_mtx);
        ekf_predict_nolock(kDt, b.imu);
        ekf_update_gnss_nolock(b.gnss);
        snap = g_ekf;  // copy state then immediately release
    }

    PerceptionResult r;
    r.objects.reserve(8);  // pre-allocate to avoid realloc on push_back
    r.cycle_id        = b.cycle_id;
    r.ego_pos_enu     = snap.pos;
    r.ego_vel         = snap.vel;
    r.ego_heading_deg = snap.heading;
    r.lane_offset_m   = b.camera.lane_offset_m;

    // Confidence: GNSS HDOP + LiDAR density + radar quality
    double gnss_q  = std::max(0.0, 1.0 - (b.gnss.hdop - 1.0) / 5.0);
    double lidar_q = std::min(1.0, b.lidar.point_count / 65536.0);
    double radar_q = (b.radar.range_m > 3.0 && b.radar.range_m < 200.0) ? 1.0 : 0.5;
    r.confidence   = 0.5*gnss_q + 0.3*lidar_q + 0.2*radar_q;
    co_return r;
}

// Stage 4: object tracking (simplified SORT)
// Uses atomic fetch_add to eliminate data races (safe even with multi-worker pipeline)
static std::atomic<uint32_t> g_track_id{0};
static Task<Result<PerceptionResult>> track_objects(PerceptionResult r, ActionEnv) {
    // Cross-validate Radar + LiDAR to track forward objects
    double radar_range = r.ego_pos_enu.x; // dummy — separate tracker in production
    (void)radar_range;

    // Example: create tracked object from forward LiDAR
    // (full SORT: Hungarian algorithm + Kalman filter per-track)
    TrackedObject obj;
    obj.id = g_track_id.fetch_add(1, std::memory_order_relaxed) % 16;
    // Object position: ahead of ego in ENU frame
    double cos_h = std::cos(r.ego_heading_deg * M_PI / 180.0);
    double sin_h = std::sin(r.ego_heading_deg * M_PI / 180.0);
    // Forward vehicle position estimate (LiDAR nearest + heading)
    double dist = std::max(3.0, r.ego_pos_enu.z > 0 ? r.ego_pos_enu.z : 8.0);
    (void)dist;
    obj.position_enu = {
        r.ego_pos_enu.x + cos_h * 12.0,
        r.ego_pos_enu.y + sin_h * 12.0,
        0.0
    };
    obj.velocity = {r.ego_vel.x * 0.8, r.ego_vel.y * 0.8, 0};

    // TTC = relative distance / closing speed
    double closing_speed = vec3_len(r.ego_vel) - vec3_len(obj.velocity);
    double separation    = 12.0;  // use lidar.nearest_m in production
    obj.ttc_sec  = (closing_speed > 0.1) ? (separation / closing_speed) : 99.0;
    obj.critical = (obj.ttc_sec < 3.0);

    r.emergency_flag = obj.critical;
    r.objects.push_back(obj);
    co_return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// §6  Sink — final perception collection
// ─────────────────────────────────────────────────────────────────────────────

// alignas(64): prevent false sharing between adjacent counters (each on its own cache line)
alignas(64) static std::atomic<int> g_perception_count{0};
alignas(64) static std::atomic<int> g_emergency_count{0};

struct PerceptionSink {
    Result<void> init() { return {}; }
    Task<Result<void>> sink(const PerceptionResult& r) {
        g_perception_count.fetch_add(1, std::memory_order_relaxed);
        const char* flag = r.emergency_flag ? " [!EMERGENCY!]" : "";
        std::println(
            "  [Perc #{:3}] pos=({:.1f},{:.1f}) vel=({:.2f},{:.2f}) "
            "lane={:.2f}m conf={:.2f} tracks={}{}",
            r.cycle_id,
            r.ego_pos_enu.x, r.ego_pos_enu.y,
            r.ego_vel.x, r.ego_vel.y,
            r.lane_offset_m,
            r.confidence,
            r.objects.size(),
            flag);
        co_return Result<void>{};
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// §7  DynamicPipeline stages — Planning / Control / Emergency
// ─────────────────────────────────────────────────────────────────────────────

alignas(64) static std::atomic<int> g_plan_count{0};
alignas(64) static std::atomic<int> g_alert_count{0};

// Obstacle collision risk calculation
static Task<Result<PerceptionResult>> obstacle_check(PerceptionResult r, ActionEnv) {
    for (auto& obj : r.objects) {
        if (obj.ttc_sec < 5.0) {
            std::println("    [obstacle] id={} TTC={:.1f}s {}",
                        obj.id, obj.ttc_sec, obj.critical ? "WARNING CRITICAL" : "caution");
            g_alert_count.fetch_add(1, std::memory_order_relaxed);
        }
    }
    co_return r;
}

// Path planning (simple Pure Pursuit)
static Task<Result<PerceptionResult>> path_plan(PerceptionResult r, ActionEnv) {
    // Lane recovery: generate steering angle when offset is large
    double steer = -0.5 * r.lane_offset_m;  // rad
    (void)steer;
    g_plan_count.fetch_add(1, std::memory_order_relaxed);
    co_return r;
}

// Velocity command generation (normal driving)
static Task<Result<PerceptionResult>> velocity_cmd_normal(PerceptionResult r, ActionEnv) {
    double target_v = 13.9;  // 50 km/h
    for (auto& obj : r.objects) {
        if (obj.ttc_sec < 5.0)
            target_v = std::min(target_v, obj.ttc_sec * 2.0);
    }
    std::println("    [velocity cmd] target={:.1f} km/h (normal)", target_v * 3.6);
    co_return r;
}

// Emergency braking — activated after hot-swap
static Task<Result<PerceptionResult>> velocity_cmd_emergency(PerceptionResult r, ActionEnv) {
    g_emergency_count.fetch_add(1, std::memory_order_relaxed);
    std::println("    [emergency braking] AEB activated! Immediate stop command (cycle #{})",
                r.cycle_id);
    co_return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// §8  Vehicle state machine
// ─────────────────────────────────────────────────────────────────────────────

enum class VehicleState { IDLE, PERCEPTION, PLANNING, CONTROL, EMERGENCY };

static const char* state_name(VehicleState s) {
    switch (s) {
    case VehicleState::IDLE:        return "IDLE";
    case VehicleState::PERCEPTION:  return "PERCEPTION";
    case VehicleState::PLANNING:    return "PLANNING";
    case VehicleState::CONTROL:     return "CONTROL";
    case VehicleState::EMERGENCY:   return "EMERGENCY";
    }
    return "?";
}

// ─────────────────────────────────────────────────────────────────────────────
// §9  Coroutine entry points
// ─────────────────────────────────────────────────────────────────────────────

static std::atomic<bool>    g_perception_done{false};
static std::atomic<bool>    g_planning_done{false};

// Use condition_variable for completion signaling instead of sleep polling
static std::mutex              g_done_mtx;
static std::condition_variable g_done_cv;

// Workaround for GCC ICE: use heap allocation to avoid large structs on coroutine frames
[[gnu::noinline]]
static std::unique_ptr<RawSensorBundle> make_bundle(int i) {
    static MockCamera     cam;
    static MockRadar      radar;
    static MockLiDAR      lidar;
    static MockIMU        imu;
    static MockGNSS       gnss;
    static MockUltrasonic ultra;

    uint64_t ts = static_cast<uint64_t>(i) * 50000ULL;
    auto b = std::make_unique<RawSensorBundle>();
    b->camera     = cam.read(ts, i);
    b->radar      = radar.read(ts, i);
    b->lidar      = lidar.read(ts, i);
    b->imu        = imu.read(ts, i);
    b->gnss       = gnss.read(ts, i);
    b->ultrasonic = ultra.read(ts, i);
    b->cycle_id   = static_cast<uint64_t>(i + 1);
    b->ts_us      = ts;
    return b;
}

static Task<void> run_perception(
    std::shared_ptr<StaticPipeline<RawSensorBundle, PerceptionResult>> pipe,
    int cycles)
{
    for (int i = 0; i < cycles; ++i) {
        auto b = make_bundle(i);
        co_await pipe->push(*b);
    }
    g_perception_done.store(true, std::memory_order_release);
    g_done_cv.notify_all();
    co_return;
}

static Task<void> run_planning(
    std::shared_ptr<DynamicPipeline<PerceptionResult>> dp,
    MessageBus& bus,
    int cycles)
{
    MockCamera     cam;
    MockRadar      radar;
    MockLiDAR      lidar;
    MockIMU        imu;
    MockGNSS       gnss;
    MockUltrasonic ultra;

    VehicleState state = VehicleState::PLANNING;
    bool swapped = false;

    for (int i = 0; i < cycles; ++i) {
        uint64_t ts = static_cast<uint64_t>(i) * 50000ULL;

        // Reproduce simplified perception result
        PerceptionResult r;
        r.cycle_id = static_cast<uint64_t>(i + 1);
        r.lane_offset_m = cam.read(ts, i).lane_offset_m;

        // Compute Radar TTC
        auto rad = radar.read(ts, i);
        double ego_speed = 13.9;  // 50 km/h
        double ttc = (rad.velocity_mps > 0.1)
            ? rad.range_m / rad.velocity_mps
            : 99.0;

        TrackedObject obj;
        obj.id       = static_cast<uint32_t>(i % 8);
        obj.ttc_sec  = ttc;
        obj.critical = (ttc < 3.0);
        r.objects.reserve(1);  // prevent realloc on push_back
        r.objects.push_back(obj);
        r.emergency_flag = obj.critical;
        r.confidence = 0.85;
        r.ego_vel    = {ego_speed, 0, 0};

        // Emergency detected → hot-swap
        if (r.emergency_flag && !swapped) {
            std::println("\n  [State Machine] {} -> EMERGENCY (TTC={:.1f}s)",
                        state_name(state), ttc);
            dp->hot_swap("velocity_cmd", velocity_cmd_emergency);
            state   = VehicleState::EMERGENCY;
            swapped = true;
        }

        dp->try_push(r);

        // Publish vehicle state to bus
        co_await bus.publish("vehicle_state", r);
    }
    g_planning_done.store(true, std::memory_order_release);
    g_done_cv.notify_all();
    co_return;
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::println("=== Autonomous driving multi-sensor fusion example ===");
    std::println("    Mock HAL: Camera | Radar | LiDAR | IMU | GNSS | Ultrasonic\n");

    constexpr int kCycles = 30;  // 30 cycles × 50ms = 1.5s simulation

    Dispatcher disp(4);
    std::jthread worker([&] { disp.run(); });

    // ── §A  MessageBus ──────────────────────────────────────────────────────
    MessageBus bus;
    bus.start(disp);

    static std::atomic<int> g_state_msgs{0};
    auto sub = bus.subscribe("vehicle_state",
        [](MessageBus::Msg msg, Context) -> Task<Result<void>> {
            try {
                auto& r = std::any_cast<PerceptionResult&>(msg);
                (void)r;
                g_state_msgs.fetch_add(1, std::memory_order_relaxed);
            } catch (...) {}
            co_return Result<void>{};
        });

    // ── §B  StaticPipeline — Perception ────────────────────────────────────
    std::println("── §A  Perception StaticPipeline configuration ──");
    std::println("       validate -> calibrate -> fuse(EKF) -> track(SORT)\n");

    auto perc_pipe = std::make_shared<StaticPipeline<RawSensorBundle, PerceptionResult>>(
        PipelineBuilder<RawSensorBundle>{}
            .add<RawSensorBundle>(validate_bundle)
            .add<RawSensorBundle>(calibrate)
            .add<PerceptionResult>(fuse_perception)
            .add<PerceptionResult>(track_objects)
            .with_sink(PerceptionSink{})
            .build());
    perc_pipe->start(disp);

    std::println("── §B  Perception loop ({} cycles, 50ms interval) ──", kCycles);
    disp.spawn(run_perception(perc_pipe, kCycles));

    {
        std::unique_lock<std::mutex> lk(g_done_mtx);
        g_done_cv.wait_for(lk, 8s,
            []{ return g_perception_done.load(std::memory_order_acquire); });
    }
    std::this_thread::sleep_for(300ms);  // wait for pipeline drain

    // ── §C  DynamicPipeline — Planning / Control ────────────────────────────
    std::println("\n── §C  Planning DynamicPipeline configuration ──");
    std::println("       obstacle_check -> path_plan -> velocity_cmd (-> hot-swap: emergency)\n");

    auto plan_dp = std::make_shared<DynamicPipeline<PerceptionResult>>();
    plan_dp->add_stage("obstacle_check", obstacle_check);
    plan_dp->add_stage("path_plan",      path_plan);
    plan_dp->add_stage("velocity_cmd",   velocity_cmd_normal);
    plan_dp->start(disp);

    disp.spawn(run_planning(plan_dp, bus, kCycles));

    {
        std::unique_lock<std::mutex> lk(g_done_mtx);
        g_done_cv.wait_for(lk, 8s,
            []{ return g_planning_done.load(std::memory_order_acquire); });
    }
    std::this_thread::sleep_for(300ms);  // wait for pipeline drain

    disp.stop();
    worker.join();

    // ── Results summary ───────────────────────────────────────────────────────
    std::println("\n==========================================");
    std::println("  Autonomous driving sensor fusion results");
    std::println("==========================================");
    std::println("  Perception processed : {} / {} cycles", g_perception_count.load(), kCycles);
    std::println("  Planning executions  : {}", g_plan_count.load());
    std::println("  Obstacle alerts      : {}", g_alert_count.load());
    std::println("  Emergency braking    : {} (AEB)", g_emergency_count.load());
    std::println("  Vehicle state msgs   : {}", g_state_msgs.load());
    std::println("  EKF position estimate: ({:.1f}, {:.1f}, {:.1f}) m",
                g_ekf.pos.x, g_ekf.pos.y, g_ekf.pos.z);
    std::println("------------------------------------------");
    std::println("  Pipeline: validate->calibrate->fuse(EKF)->track(SORT)");
    std::println("  DynamicPipeline: normal->[hot-swap]->emergency AEB");
    std::println("  MessageBus: vehicle_state topic subscribe/publish");

    bool ok = (g_perception_count.load() > 0 && g_plan_count.load() > 0);
    std::println("\nautonomous_driving_fusion: {}", ok ? "ALL OK" : "WARN — no output");
    return 0;
}
