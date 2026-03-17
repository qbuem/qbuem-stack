# autonomous_driving_fusion

**Category:** Advanced Applications
**File:** `autonomous_driving_fusion.cpp`
**Complexity:** Expert

## Overview

A comprehensive autonomous driving sensor fusion system. Fuses data from 6 sensor types (Camera, Radar, LiDAR, IMU, GNSS, Ultrasonic) through a `StaticPipeline` (validate → calibrate → EKF → SORT tracker) combined with a `DynamicPipeline` for AEB (Automatic Emergency Braking) hot-swap, all publishing vehicle state via `MessageBus`.

## Scenario

An autonomous vehicle's perception stack receives raw sensor frames from a mock HAL (Hardware Abstraction Layer). The pipeline fuses them using an Extended Kalman Filter, tracks objects with SORT, and publishes the vehicle state. The AEB decision module can be hot-swapped during runtime (e.g., switching between conservative and aggressive braking profiles).

## Architecture Diagram

```
  Sensor HAL (Mock)
  ──────────────────────────────────────────────────────────
  ┌──────────┐  ┌──────────┐  ┌──────────┐
  │ Camera   │  │  Radar   │  │  LiDAR   │
  │ 30 Hz    │  │  20 Hz   │  │  10 Hz   │
  └────┬─────┘  └────┬─────┘  └────┬─────┘
       │              │              │
  ┌────┴─────┐  ┌─────┴────┐  ┌────┴─────┐
  │   IMU    │  │   GNSS   │  │Ultrasonic│
  │  200 Hz  │  │   1 Hz   │  │  50 Hz   │
  └────┬─────┘  └─────┬────┘  └────┬─────┘
       └──────────┬───┘             │
                  ▼                 │
           SensorFrame (merged)     │
                  │                 │
                  ▼                 ▼
  ┌─────────────────────────────────────────────────────────┐
  │  StaticPipeline                                         │
  │  SensorFrame → SensorFrame → FusedFrame → TrackedFrame  │
  │                                                         │
  │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌────────┐  │
  │  │ validate │─►│calibrate │─►│  EKF     │─►│  SORT  │  │
  │  │ (reject  │  │(coord    │  │(state    │  │(object │  │
  │  │  corrupt)│  │ transform│  │ estimate)│  │ track) │  │
  │  └──────────┘  └──────────┘  └──────────┘  └────┬───┘  │
  └─────────────────────────────────────────────────┼───────┘
                                                    │
                           ┌────────────────────────┤
                           │                        │
                           ▼                        ▼
  ┌───────────────────────────────┐  ┌──────────────────────┐
  │  DynamicPipeline (AEB)        │  │  MessageBus          │
  │  TrackedFrame → AEBDecision   │  │  "vehicle_state"     │
  │  (hot-swap: conservative /    │  │  topic publish       │
  │            aggressive)        │  └──────────────────────┘
  └───────────────────────────────┘
```

## Key Pipeline Stages

| Stage | Input | Output | Purpose |
|-------|-------|--------|---------|
| `validate` | `SensorFrame` | `SensorFrame` | Reject corrupt / out-of-range sensor data |
| `calibrate` | `SensorFrame` | `SensorFrame` | Transform to vehicle coordinate frame |
| `ekf_fuse` | `SensorFrame` | `FusedFrame` | Extended Kalman Filter state estimation |
| `sort_track` | `FusedFrame` | `TrackedFrame` | SORT multi-object tracker |
| `aeb_decision` (dynamic) | `TrackedFrame` | `AEBDecision` | Emergency braking logic (hot-swappable) |

## Key APIs Used

| API | Purpose |
|-----|---------|
| `StaticPipeline` | Compile-time type-checked pipeline |
| `DynamicPipeline<T>` | Runtime AEB hot-swap |
| `MessageBus::publish("vehicle_state", ...)` | Fan-out vehicle state to subscribers |
| `dp.hot_swap("aeb", new_profile)` | Switch AEB profile without stopping |
| `Dispatcher` | Multi-thread reactor |

## Input

Mock sensor frames at their respective rates (simulated via a timer loop):
- `CameraFrame{width, height, pixels[]}`
- `RadarReturn{range, velocity, azimuth}`
- `LidarPointCloud{points[], timestamp}`
- `ImuData{accel_xyz, gyro_xyz, timestamp}`
- `GnssData{lat, lon, alt, accuracy}`
- `UltrasonicDistance{distances[8]}`

## Output

```
[EKF] state: pos=(12.3, 0.1, 0.0) vel=(11.2, 0.0, 0.0) heading=0.01rad
[SORT] tracked objects: 3 (id=1 car, id=2 pedestrian, id=3 cyclist)
[AEB] no_brake (nearest=15m, ttc=1.3s)
[MessageBus] vehicle_state published: speed=11.2 m/s heading=0.01rad
```

## How to Run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target autonomous_driving_fusion
./build/examples/11-advanced-apps/autonomous_driving/autonomous_driving_fusion
```

## Notes

- All sensor data is mock-generated; no real hardware is required.
- The EKF implementation uses a simplified 6-DOF state vector; production systems use more sophisticated models.
- AEB hot-swap demonstrates zero-downtime profile switching — critical for OTA updates in production vehicles.
