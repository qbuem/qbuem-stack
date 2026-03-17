/**
 * @file autonomous_driving_fusion.cpp
 * @brief 자율주행 멀티센서 퓨전 예제
 *
 * ## 개요
 * 실제 하드웨어 없이 Mock HAL 레이어로 6종 센서를 시뮬레이션합니다.
 * qbuem StaticPipeline + DynamicPipeline + MessageBus를 활용해
 * Perception → Planning → Control 파이프라인을 구현합니다.
 *
 * ## 아키텍처
 * ```
 * ┌─────────────────────────── Mock HAL Layer ──────────────────────────────┐
 * │  MockCamera  MockRadar  MockLiDAR  MockIMU  MockGPS  MockUltrasonic     │
 * └─────────────────────┬───────────────────────────────────────────────────┘
 *                        │ RawSensorBundle
 *                        ▼
 *          ┌─────────────────────────────────────────┐
 *          │   StaticPipeline<RawSensorBundle,        │
 *          │                  PerceptionResult>        │
 *          │  1. validate_bundle()  — 센서 타임스탬프 정합성  │
 *          │  2. calibrate()        — 외부 보정 행렬 적용    │
 *          │  3. fuse_perception()  — EKF 스타일 가중 퓨전   │
 *          │  4. track_objects()    — 장애물 추적 + SORT     │
 *          └─────────────────┬───────────────────────┘
 *                             │ PerceptionResult
 *                             ▼
 *          ┌─────────────────────────────────────────┐
 *          │   DynamicPipeline<PerceptionResult>      │
 *          │  "obstacle_check" — 충돌 위험도 계산       │
 *          │  "path_plan"      — 목표 경로 생성         │
 *          │  "velocity_cmd"   — 속도/조향 명령 생성     │
 *          │  "emergency"      — 긴급 제동 판단 (hotswap) │
 *          └─────────────────┬───────────────────────┘
 *                             │
 *                    MessageBus("vehicle_state")
 *                    MessageBus("alerts")
 *
 * ## 차량 상태 머신
 *   IDLE → PERCEPTION → PLANNING → CONTROL → EMERGENCY(hot-swap)
 *
 * ## 커버리지
 * - Mock HAL (Camera, Radar, LiDAR, IMU, GPS, Ultrasonic)
 * - StaticPipeline<In, Out> + PipelineBuilder + with_sink()
 * - DynamicPipeline<T> + hot_swap() (normal → emergency)
 * - MessageBus: publish / subscribe
 * - Dispatcher + Task<T> + 코루틴 수명 관리
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/dynamic_pipeline.hpp>
#include <qbuem/pipeline/message_bus.hpp>
#include <qbuem/pipeline/static_pipeline.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace qbuem;
using namespace std::chrono_literals;

// ─────────────────────────────────────────────────────────────────────────────
// §0  기본 수학 타입
// ─────────────────────────────────────────────────────────────────────────────

struct Vec2 { double x{0}, y{0}; };
struct Vec3 { double x{0}, y{0}, z{0}; };

inline double vec3_len(Vec3 v) {
    return std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
}

// ─────────────────────────────────────────────────────────────────────────────
// §1  센서 데이터 타입 (HAL 출력 구조체)
// ─────────────────────────────────────────────────────────────────────────────

struct CameraFrame {
    uint32_t width{1920}, height{1080};
    uint8_t  lane_detected{1};
    double   lane_offset_m{0.0};    ///< 차선 중심 대비 횡방향 오프셋
    double   forward_clearance_m{50.0};
    uint64_t ts_us{0};
};

struct RadarTrack {
    double   range_m{30.0};
    double   velocity_mps{0.0};    ///< 상대 속도 (양수 = 접근)
    double   azimuth_deg{0.0};
    uint64_t ts_us{0};
};

struct LidarCloud {
    uint32_t point_count{32768};
    double   nearest_m{8.0};       ///< 전방 최근접 포인트
    Vec3     centroid{0, 0, 1.5};  ///< 클러스터 무게중심
    uint64_t ts_us{0};
};

struct ImuSample {
    Vec3     accel{0, 0, 9.81};    ///< m/s²
    Vec3     gyro{};               ///< rad/s
    double   heading_deg{0.0};     ///< 절대 방위각
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
    double   front_m{3.5};        ///< 초음파 전방 거리
    double   rear_m{5.0};
    double   left_m{1.8};
    double   right_m{1.8};
    uint64_t ts_us{0};
};

// 퓨전 파이프라인 입력 — 한 사이클에 모든 센서 묶음
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
// §2  퍼셉션 결과 타입
// ─────────────────────────────────────────────────────────────────────────────

struct TrackedObject {
    uint32_t id{0};
    Vec3     position_enu;  ///< East-North-Up 좌표 (m)
    Vec3     velocity;      ///< m/s
    double   ttc_sec{99};   ///< Time-To-Collision
    bool     critical{false};
};

struct PerceptionResult {
    Vec3     ego_pos_enu;          ///< 자차 위치
    Vec3     ego_vel;              ///< 자차 속도
    double   ego_heading_deg{0};
    double   lane_offset_m{0};
    std::vector<TrackedObject> objects;
    double   confidence{0};
    bool     emergency_flag{false};
    uint64_t cycle_id{0};
};

// ─────────────────────────────────────────────────────────────────────────────
// §3  Mock HAL — 실제 하드웨어 없이 물리 시뮬레이션 데이터 생성
// ─────────────────────────────────────────────────────────────────────────────

struct MockCamera {
    CameraFrame read(uint64_t ts_us, int cycle) const {
        double t = ts_us * 1e-6;
        // 차선이탈: cycle 12~18 구간에서 점차 이탈
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
        // cycle 20~25: 앞차가 갑자기 감속 (접근 속도 급증)
        double rel_vel = (cycle >= 20 && cycle <= 25)
            ? 8.0 + 2.0 * (cycle - 20)   // 최대 18 m/s 접근
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
// §4  EKF 스타일 상태 추정기 (경량 버전)
// ─────────────────────────────────────────────────────────────────────────────

struct EKFState {
    Vec3   pos{};
    Vec3   vel{};
    double heading{0};
    double pos_var{1.0};  ///< 위치 분산 (신뢰도 역수)
    double vel_var{0.5};
};

// g_ekf_mtx 로 모든 EKF 상태 접근을 보호합니다.
// fuse_perception 이 co_await 전에 mutex 를 해제하므로
// 코루틴 프레임에 mutex 가 올라가지 않습니다.
static std::mutex  g_ekf_mtx;
static EKFState    g_ekf{};

// ── mutex 미획득 내부 헬퍼 (항상 g_ekf_mtx 보유 상태에서만 호출) ──

static void ekf_predict_nolock(double dt, const ImuSample& imu) {
    // 속도 적분 (가속도 기반)
    g_ekf.vel.x += imu.accel.x * dt;
    g_ekf.vel.y += imu.accel.y * dt;
    // 위치 적분
    g_ekf.pos.x += g_ekf.vel.x * dt;
    g_ekf.pos.y += g_ekf.vel.y * dt;
    g_ekf.heading = imu.heading_deg;
    // 예측 분산 증가
    g_ekf.pos_var += 0.1 * dt;
    g_ekf.vel_var += 0.05 * dt;
}

static void ekf_update_gnss_nolock(const GnssPosition& gnss) {
    constexpr double kMpD = 111320.0;
    constexpr double kLat0 = 37.5665, kLon0 = 126.9780;
    double obs_x = (gnss.lon - kLon0) * kMpD * std::cos(kLat0 * M_PI / 180.0);
    double obs_y = (gnss.lat - kLat0) * kMpD;
    double R = gnss.hdop * 2.0;  // GNSS 노이즈 공분산
    double K = g_ekf.pos_var / (g_ekf.pos_var + R);  // 칼만 게인
    g_ekf.pos.x += K * (obs_x - g_ekf.pos.x);
    g_ekf.pos.y += K * (obs_y - g_ekf.pos.y);
    g_ekf.pos.z  = gnss.alt;
    g_ekf.pos_var *= (1.0 - K);  // 업데이트 후 분산 감소
}

// ─────────────────────────────────────────────────────────────────────────────
// §5  StaticPipeline 스테이지 함수
// ─────────────────────────────────────────────────────────────────────────────

// Stage 1: 타임스탬프 정합성 검사
static Task<Result<RawSensorBundle>> validate_bundle(RawSensorBundle b, ActionEnv) {
    constexpr uint64_t kMaxJitter_us = 50000;  // 50ms
    uint64_t ref = b.ts_us;
    if (std::abs((int64_t)(b.camera.ts_us    - ref)) > (int64_t)kMaxJitter_us ||
        std::abs((int64_t)(b.radar.ts_us     - ref)) > (int64_t)kMaxJitter_us ||
        std::abs((int64_t)(b.lidar.ts_us     - ref)) > (int64_t)kMaxJitter_us) {
        // 타임스탬프 불일치 — 보정
        b.camera.ts_us     = ref;
        b.radar.ts_us      = ref;
        b.lidar.ts_us      = ref;
        b.imu.ts_us        = ref;
        b.gnss.ts_us       = ref;
        b.ultrasonic.ts_us = ref;
    }
    co_return b;
}

// Stage 2: 외부 보정 (카메라 왜곡, 레이더 바이어스 제거)
static Task<Result<RawSensorBundle>> calibrate(RawSensorBundle b, ActionEnv) {
    // Camera: 렌즈 왜곡 보정 (오프셋 바이어스 -0.01m)
    b.camera.lane_offset_m -= 0.01;
    // Radar: 반경 바이어스 보정 (+0.3m)
    b.radar.range_m += 0.3;
    // IMU: 중력 성분 제거 (z축 9.81 빼기)
    b.imu.accel.z -= 9.81;
    co_return b;
}

// Stage 3: EKF 퓨전 — GNSS + IMU 상태 추정
static Task<Result<PerceptionResult>> fuse_perception(RawSensorBundle b, ActionEnv) {
    constexpr double kDt = 0.05;  // 50ms 사이클

    // mutex 보호 구간: predict + update + snapshot 을 원자적으로 수행.
    // co_await 이전에 lock 을 해제해 mutex 가 코루틴 프레임에 올라가지 않습니다.
    EKFState snap;
    {
        std::lock_guard<std::mutex> lk(g_ekf_mtx);
        ekf_predict_nolock(kDt, b.imu);
        ekf_update_gnss_nolock(b.gnss);
        snap = g_ekf;  // 상태 복사 후 즉시 해제
    }

    PerceptionResult r;
    r.objects.reserve(8);  // 최대 추적 객체 수 사전 할당 — push_back 재할당 방지
    r.cycle_id        = b.cycle_id;
    r.ego_pos_enu     = snap.pos;
    r.ego_vel         = snap.vel;
    r.ego_heading_deg = snap.heading;
    r.lane_offset_m   = b.camera.lane_offset_m;

    // 신뢰도: GNSS HDOP + LiDAR 밀도 + 레이더 품질
    double gnss_q  = std::max(0.0, 1.0 - (b.gnss.hdop - 1.0) / 5.0);
    double lidar_q = std::min(1.0, b.lidar.point_count / 65536.0);
    double radar_q = (b.radar.range_m > 3.0 && b.radar.range_m < 200.0) ? 1.0 : 0.5;
    r.confidence   = 0.5*gnss_q + 0.3*lidar_q + 0.2*radar_q;
    co_return r;
}

// Stage 4: 객체 추적 (SORT 간소화 버전)
// atomic fetch_add 로 data race 제거 (pipeline 이 멀티워커일 때도 안전)
static std::atomic<uint32_t> g_track_id{0};
static Task<Result<PerceptionResult>> track_objects(PerceptionResult r, ActionEnv) {
    // Radar + LiDAR 교차 검증으로 전방 객체 추적
    double radar_range = r.ego_pos_enu.x; // dummy — 실제론 별도 트래커
    (void)radar_range;

    // 예시: 전방 LiDAR 기반 추적 객체 생성
    // (실제 SORT: 헝가리안 알고리즘 + 칼만 필터 per-track)
    TrackedObject obj;
    obj.id = g_track_id.fetch_add(1, std::memory_order_relaxed) % 16;
    // 객체 위치: ENU 기준 자차 전방
    double cos_h = std::cos(r.ego_heading_deg * M_PI / 180.0);
    double sin_h = std::sin(r.ego_heading_deg * M_PI / 180.0);
    // 전방 차량 위치 추정 (LiDAR nearest + heading)
    double dist = std::max(3.0, r.ego_pos_enu.z > 0 ? r.ego_pos_enu.z : 8.0);
    (void)dist;
    obj.position_enu = {
        r.ego_pos_enu.x + cos_h * 12.0,
        r.ego_pos_enu.y + sin_h * 12.0,
        0.0
    };
    obj.velocity = {r.ego_vel.x * 0.8, r.ego_vel.y * 0.8, 0};

    // TTC = 상대 거리 / 접근 속도
    double closing_speed = vec3_len(r.ego_vel) - vec3_len(obj.velocity);
    double separation    = 12.0;  // 실제론 lidar.nearest_m
    obj.ttc_sec  = (closing_speed > 0.1) ? (separation / closing_speed) : 99.0;
    obj.critical = (obj.ttc_sec < 3.0);

    r.emergency_flag = obj.critical;
    r.objects.push_back(obj);
    co_return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// §6  Sink — 퍼셉션 최종 수집
// ─────────────────────────────────────────────────────────────────────────────

// alignas(64): 인접 카운터 간 false sharing 방지 (각 카운터가 독립된 캐시라인 점유)
alignas(64) static std::atomic<int> g_perception_count{0};
alignas(64) static std::atomic<int> g_emergency_count{0};

struct PerceptionSink {
    Result<void> init() { return {}; }
    Task<Result<void>> sink(const PerceptionResult& r) {
        g_perception_count.fetch_add(1, std::memory_order_relaxed);
        const char* flag = r.emergency_flag ? " [!긴급!]" : "";
        std::printf(
            "  [Perc #%3llu] pos=(%.1f,%.1f) vel=(%.2f,%.2f) "
            "lane=%.2fm conf=%.2f tracks=%zu%s\n",
            (unsigned long long)r.cycle_id,
            r.ego_pos_enu.x, r.ego_pos_enu.y,
            r.ego_vel.x, r.ego_vel.y,
            r.lane_offset_m,
            r.confidence,
            r.objects.size(),
            flag);
        co_return Result<void>::ok();
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// §7  DynamicPipeline 스테이지 — Planning / Control / Emergency
// ─────────────────────────────────────────────────────────────────────────────

alignas(64) static std::atomic<int> g_plan_count{0};
alignas(64) static std::atomic<int> g_alert_count{0};

// 장애물 위험도 계산
static Task<Result<PerceptionResult>> obstacle_check(PerceptionResult r, ActionEnv) {
    for (auto& obj : r.objects) {
        if (obj.ttc_sec < 5.0) {
            std::printf("    [장애물] id=%u TTC=%.1fs %s\n",
                        obj.id, obj.ttc_sec, obj.critical ? "⚠ CRITICAL" : "주의");
            g_alert_count.fetch_add(1, std::memory_order_relaxed);
        }
    }
    co_return r;
}

// 경로 계획 (간단 Pure Pursuit)
static Task<Result<PerceptionResult>> path_plan(PerceptionResult r, ActionEnv) {
    // 차선 복귀 보정: offset이 크면 조향각 생성
    double steer = -0.5 * r.lane_offset_m;  // rad
    (void)steer;
    g_plan_count.fetch_add(1, std::memory_order_relaxed);
    co_return r;
}

// 속도 명령 생성 (정상 주행)
static Task<Result<PerceptionResult>> velocity_cmd_normal(PerceptionResult r, ActionEnv) {
    double target_v = 13.9;  // 50 km/h
    for (auto& obj : r.objects) {
        if (obj.ttc_sec < 5.0)
            target_v = std::min(target_v, obj.ttc_sec * 2.0);
    }
    std::printf("    [속도 명령] target=%.1f km/h (정상)\n", target_v * 3.6);
    co_return r;
}

// 긴급 제동 — hot-swap 후 활성화
static Task<Result<PerceptionResult>> velocity_cmd_emergency(PerceptionResult r, ActionEnv) {
    g_emergency_count.fetch_add(1, std::memory_order_relaxed);
    std::printf("    [긴급 제동] AEB 작동! 즉시 정차 명령 (사이클 #%llu)\n",
                (unsigned long long)r.cycle_id);
    co_return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// §8  차량 상태 머신
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
// §9  코루틴 엔트리 포인트
// ─────────────────────────────────────────────────────────────────────────────

static std::atomic<bool>    g_perception_done{false};
static std::atomic<bool>    g_planning_done{false};

// sleep 폴링 대신 condition_variable 로 완료 신호 — CPU 대기 시간 최소화
static std::mutex              g_done_mtx;
static std::condition_variable g_done_cv;

// GCC ICE 우회: 코루틴 프레임에 큰 구조체를 올리지 않도록 힙 할당 사용
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

        // 퍼셉션 결과 간단 재현
        PerceptionResult r;
        r.cycle_id = static_cast<uint64_t>(i + 1);
        r.lane_offset_m = cam.read(ts, i).lane_offset_m;

        // Radar TTC 계산
        auto rad = radar.read(ts, i);
        double ego_speed = 13.9;  // 50 km/h
        double ttc = (rad.velocity_mps > 0.1)
            ? rad.range_m / rad.velocity_mps
            : 99.0;

        TrackedObject obj;
        obj.id       = static_cast<uint32_t>(i % 8);
        obj.ttc_sec  = ttc;
        obj.critical = (ttc < 3.0);
        r.objects.reserve(1);  // push_back 시 재할당 방지
        r.objects.push_back(obj);
        r.emergency_flag = obj.critical;
        r.confidence = 0.85;
        r.ego_vel    = {ego_speed, 0, 0};

        // 긴급 상황 감지 → hot-swap
        if (r.emergency_flag && !swapped) {
            std::printf("\n  [State Machine] %s → EMERGENCY (TTC=%.1fs)\n",
                        state_name(state), ttc);
            dp->hot_swap("velocity_cmd", velocity_cmd_emergency);
            state   = VehicleState::EMERGENCY;
            swapped = true;
        }

        dp->try_push(r);

        // 차량 상태 버스 발행
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
    std::printf("=== 자율주행 멀티센서 퓨전 예제 ===\n");
    std::printf("    Mock HAL: Camera | Radar | LiDAR | IMU | GNSS | Ultrasonic\n\n");

    constexpr int kCycles = 30;  // 30 사이클 × 50ms = 1.5초 시뮬레이션

    Dispatcher disp(4);
    std::thread worker([&] { disp.run(); });

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
            co_return Result<void>::ok();
        });

    // ── §B  StaticPipeline — Perception ────────────────────────────────────
    std::printf("── §A  Perception StaticPipeline 구성 ──\n");
    std::printf("       validate → calibrate → fuse(EKF) → track(SORT)\n\n");

    auto perc_pipe = std::make_shared<StaticPipeline<RawSensorBundle, PerceptionResult>>(
        PipelineBuilder<RawSensorBundle>{}
            .add<RawSensorBundle>(validate_bundle)
            .add<RawSensorBundle>(calibrate)
            .add<PerceptionResult>(fuse_perception)
            .add<PerceptionResult>(track_objects)
            .with_sink(PerceptionSink{})
            .build());
    perc_pipe->start(disp);

    std::printf("── §B  Perception 루프 (%d 사이클, 50ms 간격) ──\n", kCycles);
    disp.spawn(run_perception(perc_pipe, kCycles));

    {
        std::unique_lock<std::mutex> lk(g_done_mtx);
        g_done_cv.wait_for(lk, 8s,
            []{ return g_perception_done.load(std::memory_order_acquire); });
    }
    std::this_thread::sleep_for(300ms);  // 파이프라인 drain 대기

    // ── §C  DynamicPipeline — Planning / Control ────────────────────────────
    std::printf("\n── §C  Planning DynamicPipeline 구성 ──\n");
    std::printf("       obstacle_check → path_plan → velocity_cmd (→ hot-swap: emergency)\n\n");

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
    std::this_thread::sleep_for(300ms);  // 파이프라인 drain 대기

    disp.stop();
    worker.join();

    // ── 결과 요약 ────────────────────────────────────────────────────────────
    std::printf("\n══════════════════════════════════════════\n");
    std::printf("  자율주행 센서 퓨전 결과 요약\n");
    std::printf("══════════════════════════════════════════\n");
    std::printf("  Perception 처리   : %d / %d 사이클\n", g_perception_count.load(), kCycles);
    std::printf("  Planning 계획     : %d 회\n",          g_plan_count.load());
    std::printf("  장애물 경보       : %d 건\n",          g_alert_count.load());
    std::printf("  긴급 제동 발동    : %d 회 (AEB)\n",    g_emergency_count.load());
    std::printf("  Vehicle State 메시지: %d 건\n",        g_state_msgs.load());
    std::printf("  EKF 위치 추정     : (%.1f, %.1f, %.1f) m\n",
                g_ekf.pos.x, g_ekf.pos.y, g_ekf.pos.z);
    std::printf("──────────────────────────────────────────\n");
    std::printf("  Pipeline: validate→calibrate→fuse(EKF)→track(SORT)\n");
    std::printf("  DynamicPipeline: normal→[hot-swap]→emergency AEB\n");
    std::printf("  MessageBus: vehicle_state 토픽 구독/발행\n");

    bool ok = (g_perception_count.load() > 0 && g_plan_count.load() > 0);
    std::printf("\nautonomous_driving_fusion: %s\n", ok ? "ALL OK" : "WARN — 출력 없음");
    return 0;
}
