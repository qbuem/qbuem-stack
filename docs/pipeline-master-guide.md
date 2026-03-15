# qbuem-stack Pipeline System: The Complete Guide

This is the definitive reference for the `qbuem-stack` Pipeline system. It covers everything from core architectural principles to advanced orchestration and real-world implementation recipes.

---

## 1. Overview & Core Philosophy

The `qbuem-stack` Pipeline system is built on the same "Three Zeros" philosophy as the rest of the library: **Zero Latency, Zero Allocation, and Zero Dependency**.

### Key Principles:
- **Lock-Free by Default**: Utilizes C++20 coroutines and MPMC/SPSC ring buffers to eliminate traditional mutex-based synchronization.
- **Worker Isolation (Bulkheading)**: Prevents "heavy" operations from blocking the entire system by assigning independent worker pools to each processing stage.
- **Natural Backpressure**: Suspends producers at the language level (`co_await`) when consumers are saturated, preventing memory bloat without complex flow-control logic.
- **Mechanical Sympathy**: NUMA-aware scheduling and cache-aligned structures ensure maximum hardware utilization.

---

## 2. System Architecture

The pipeline system is organized into a 5-layer stack, with the kernel-native `Reactor` at the base.

### Layer 0: Reactor & Dispatcher
The `Reactor` (Epoll, Kqueue, or io_uring) handles low-level I/O. The `Dispatcher` manages worker threads and `spawn()`s coroutines.
- `Reactor::post(fn)`: Safely injects tasks from other threads.
- `Dispatcher::spawn(task)`: Launches a coroutine-based worker onto a reactor.

### Layer 1: AsyncChannel<T>
The MPMC Ring Buffer that serves as the "glue" between actions.
- **Fixed Capacity**: Ensures deterministic memory usage.
- **Wait Lists**: Uses intrusive lists to track suspended coroutines waiting for space (send) or data (recv).

### Layer 2: Action<In, Out>
The fundamental unit of processing.
- **Worker Pool**: Each Action manages a set of coroutine workers.
- **Environment**: `ActionEnv` provides access to Item Context, Stop Tokens, and the Service Registry.

---

## 3. Static vs. Dynamic Pipelines

`qbuem-stack` provides two distinct ways to build pipelines, balancing performance and flexibility.

### 3-1. StaticPipeline<In, Out>
- **Mechanism**: Compiled as a fixed type-chain.
- **Best For**: Performance-critical paths (e.g., HTTP request processing).
- **Pros**: Zero overhead, full compiler inlining, compile-time type safety.
- **Cons**: Structure cannot change without re-compilation.

### 3-2. DynamicPipeline<T>
- **Mechanism**: Uses runtime type erasure (`std::any`) and polymorphic interfaces.
- **Best For**: ETL workflows, config-driven logic, or systems requiring **Hot-swapping**.
- **Pros**: Stage addition/removal at runtime, Hot-swapping without stopping the world.
- **Cons**: Minor overhead from virtual calls.

---

## 4. Performance & Scheduling

Handling mixed workloads (Light vs. Heavy) is a core strength of `qbuem-stack`.

### 4-1. Bulkheading
By giving heavy actions (e.g., NPU inference) their own worker pools, we ensure that a bottleneck in processing doesn't starve the lightweight stages (e.g., JSON parsing).

### 4-2. Non-blocking Yielding
When an action worker waits for I/O or a hardware lock, it `co_await`s, yielding the CPU to other workers. This ensures that a single blocked stage doesn't halt the entire reactor thread.

---

## 5. Pattern Catalog: Topology & Flow

### 5-1. Topological Patterns
- **Fan-out (Broadcast)**: Splitting one flow into multiple downstream paths (e.g., Main logic + Audit log).
- **Fan-in (Merge)**: Collecting output from multiple sources into a single sink.
- **DAG (Directed Acyclic Graph)**: Complex non-linear flows managed by `PipelineGraph`.

### 5-2. Messaging Patterns
- **Feedback Loop**: Sending results back to an upstream stage for retry or recursive processing.
- **Sidecar Observation**: Attaching a "T-pipe" observer that copies data for monitoring without affecting the main latency path.

---

## 6. Applied Recipes (Real-World Solutions)

### Recipe A: Sensor Fusion (N:1 Sync)
Synchronizing IMU and GPS data.
1.  Use a `Gather` Action that stores partial data in the `ServiceRegistry`.
2.  Once all parts arrive (aligned by Context ID), compute the fusion and emit the result.

### Recipe B: Hardware Batching (NPU)
Collecting individual frames to maximize NPU throughput.
1.  Action uses `WorkerLocal` storage to accumulate 8 frames.
2.  On the 8th frame, emit the batch and clear the buffer.

### Recipe C: Resilient WAS (Dead Letter Queue)
1.  An Action catches logic/timeout errors.
2.  Failed items are pushed to a dedicated "DLQ Pipeline" for offline analysis/retry.

---

## 7. Periodic Polling & Sources

For "Push" style ingestion (Sensors, Hardware registers):
- **Pattern**: A `while(true)` coroutine loop.
- **Mechanism**: `co_await qbuem::sleep(ms)` + `pipeline.push(data)`.
- **Scaling**: Pin these sources to specific reactors using `Dispatcher::spawn_on(core_idx, task)`.

---

*qbuem-stack — The ultimate infrastructure for high-performance data engineering.*
