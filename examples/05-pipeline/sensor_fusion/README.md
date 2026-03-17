# pipeline_sensor_fusion

**Category:** Pipeline
**File:** `pipeline_sensor_fusion.cpp`
**Complexity:** Advanced
**Guide Reference:** Pipeline Master Guide §6A (Recipe A)

## Overview

Demonstrates **N:1 synchronous gather** using `ServiceRegistry` inside a pipeline. Multiple sensor streams (IMU, GPS, LiDAR) publish their latest readings to a service registry; a downstream pipeline stage gathers all N readings synchronously before emitting a fused state vector.

## Scenario

An autonomous robot controller fuses data from three sensors. Each sensor publishes at its own rate. A fusion stage collects the latest reading from all three, aligns them, and produces a fused state vector for the motion planner.

## Architecture Diagram

```
  ┌──────────┐  register("imu")   ┌──────────────────────────┐
  │ IMU      │ ──────────────────► │                          │
  └──────────┘                     │  ServiceRegistry         │
  ┌──────────┐  register("gps")   │  (latest value per key)  │
  │ GPS      │ ──────────────────► │                          │
  └──────────┘                     │                          │
  ┌──────────┐  register("lidar") │                          │
  │ LiDAR    │ ──────────────────► │                          │
  └──────────┘                     └──────────┬───────────────┘
                                               │
                            N:1 gather (sync)  │
                                               ▼
                                    ┌──────────────────────┐
                                    │  FusionAction         │
                                    │  collect(imu, gps,    │
                                    │          lidar)       │
                                    └──────────┬────────────┘
                                               │
                                               ▼
                                    FusedStateVector
                                    (position, velocity,
                                     orientation)
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `ServiceRegistry` | Key-value store for latest sensor values |
| `registry.register_service(name, provider)` | Register a value provider |
| `registry.get<T>(name)` | Synchronously retrieve latest value |
| `StaticPipeline` with `add<U>(fn)` | Pipeline with typed stage chaining |

## Input

| Sensor | Rate | Data |
|--------|------|------|
| IMU | 200 Hz | `{accel_x, accel_y, accel_z, gyro_x, gyro_y, gyro_z}` |
| GPS | 10 Hz | `{lat, lon, altitude, hdop}` |
| LiDAR | 20 Hz | `{points[], timestamp}` |

## Output

```
FusedStateVector {
  position: {x, y, z},
  velocity: {vx, vy, vz},
  orientation: {roll, pitch, yaw},
  confidence: 0.0–1.0
}
```

## How to Run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target pipeline_sensor_fusion
./build/examples/05-pipeline/sensor_fusion/pipeline_sensor_fusion
```

## Notes

- The `ServiceRegistry` pattern avoids synchronization overhead by storing only the latest value; perfect for sensors with different update rates.
- For more complex fusion (EKF, UKF), see `autonomous_driving_fusion` in `11-advanced-apps`.
