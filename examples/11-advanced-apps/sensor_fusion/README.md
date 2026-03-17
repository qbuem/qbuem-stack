# sensor_fusion

**Category:** Advanced Applications
**File:** `sensor_fusion_example.cpp`
**Complexity:** Advanced

## Overview

Demonstrates a general-purpose sensor fusion pipeline: mock IMU + GPS + LiDAR sensors feed a `StaticPipeline` for raw fusion, then a `DynamicPipeline` for adaptive post-processing. Simpler than `autonomous_driving_fusion` — focuses on the pipeline mechanics without full autonomous driving complexity.

## Scenario

A robotics platform fuses 3 sensor streams into a unified pose estimate. The fusion is divided into two stages: a static pipeline for validated calibration fusion, and a dynamic pipeline for adaptive filtering that can be tuned at runtime.

## Architecture Diagram

```
  Sensor Mock Sources
  ──────────────────────────────────────────────────────────
  IMU (100 Hz)    GPS (1 Hz)    LiDAR (10 Hz)
      │               │              │
      └───────────────┴──────────────┘
                      │
                      ▼ SensorBundle {imu, gps, lidar, timestamp}
  ┌─────────────────────────────────────────────────────────┐
  │  StaticPipeline                                         │
  │  SensorBundle → ValidatedBundle → CalibratedBundle      │
  │                                → FusedPose              │
  │                                                         │
  │  [validate]─►[calibrate]─►[fuse]                        │
  └──────────────────────────┬──────────────────────────────┘
                             │  FusedPose
                             ▼
  ┌─────────────────────────────────────────────────────────┐
  │  DynamicPipeline (adaptive)                             │
  │  FusedPose → FilteredPose                               │
  │  [smooth] ← hot-swap ← [aggressive_filter]             │
  └──────────────────────────┬──────────────────────────────┘
                             │
                             ▼
                     Final pose output
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `StaticPipeline` | Type-safe 3-stage validation + fusion |
| `DynamicPipeline<T>` | Runtime-swappable adaptive filter |
| `dp.hot_swap(name, fn)` | Switch filter algorithm |

## Input

```cpp
SensorBundle {
  imu:   {accel_xyz, gyro_xyz}  @ 100 Hz
  gps:   {lat, lon, alt}        @ 1 Hz
  lidar: {point_count, centroid} @ 10 Hz
  timestamp_ns: uint64_t
}
```

## Output

```
FusedPose {
  position: {x, y, z}    (meters)
  orientation: {roll, pitch, yaw}  (radians)
  velocity: {vx, vy, vz}  (m/s)
  covariance: 6×6 matrix
  confidence: 0.0–1.0
}
```

## How to Run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target sensor_fusion_example
./build/examples/11-advanced-apps/sensor_fusion/sensor_fusion_example
```

## Notes

- This is a simplified version of the full autonomous driving pipeline — ideal as an introduction before tackling `autonomous_driving_fusion`.
- The dynamic pipeline's hot-swap demonstrates switching between a Kalman smoother and a complementary filter without stopping data flow.
