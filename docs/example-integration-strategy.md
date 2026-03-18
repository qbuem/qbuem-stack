# Example Integration Strategy for High-Performance Primitives

This document outlines how the v2.4.0 primitives will be demonstrated through existing and new examples to showcase the "Zero Latency" philosophy.

## 1. Refactoring Existing Examples

### 1-1. Advanced Game Server (`examples/11-advanced-apps/game_server/`)
The current game server uses a synchronized `std::unordered_map` which limits scaling.
- **Optimization**: Replace `std::mutex mtx_` in `GameRegistry` with `LockFreeHashMap`.
- **Spatial Addition**: Introduce a spatial layer where player visibility is managed by `GridBitset` and `AOIManager`.
- **Precision Loop**: Replace the auto-scaler's `std::thread::sleep` with a `MicroTicker` driving a `60Hz` game heartbeat.
- **Safety**: Use `GenerationPool` to issue `Handle<Room>` instead of raw `uint64_t` IDs.

### 1-2. Foundation Async Timer (`examples/01-foundation/async_timer/`)
Upgrade the example to show how `MicroTicker` can drive a `Reactor` with sub-millisecond precision compared to the standard `poll()` wait.

---

## 2. New Specialized Examples

### 2-1. HFT Matching Engine (`examples/11-advanced-apps/hft_matching/`)
A new high-end application showing the stack's applicability to financial systems.
- **Lock-Free Order Book**: Uses `LockFreeHashMap` for fast symbol lookup and `IntrusiveList` for $O(1)$ priority-level management.
- **Deterministic Feed**: Uses `MicroTicker` to process incoming price updates at a fixed $100\mu s$ interval.

### 2-2. Spatial Sensor Fusion (`examples/11-advanced-apps/spatial_fusion/`)
Demonstrates the 2.5D/3D capabilities of `GridBitset`.
- **Scene**: A 3D volumetric space partitioned into voxels.
- **Query**: High-speed "find-in-sphere" queries using SIMD to detect collisions or obstacles in a simulated point cloud.

### 2-3. Lock-Free Stress Test (`examples/03-memory/lockfree_bench/`)
A pure performance benchmark comparing:
- `std::mutex` + `std::unordered_map` vs. `LockFreeHashMap`.
- `std::list` vs. `IntrusiveList`.
- `std::vector<shared_ptr>` vs. `GenerationPool`.

---

## 3. Implementation Checklist
- [ ] Implement `micro_ticker_example.cpp` in `01-foundation`.
- [ ] Add `spatial_fusion` to `11-advanced-apps`.
- [ ] Refactor `game_server.cpp` to use `LockFreeHashMap` and `AOIManager`.
- [ ] Create `lockfree_bench` to prove the performance gains.
