# qbuem-stack: epoll Optimization Guide (Legacy Linux)

This guide outlines advanced techniques for the `epoll` reactor to achieve sub-microsecond event loop latency on systems where `io_uring` is not available or for specialized low-latency networking.

## 1. The Scaling Strategy

### 1.1 Edge-Triggered (`EPOLLET`)
- **Strategy**: Use `EPOLLET` mode for all socket events. 
- **Requirement**: **Draining**. You must read/write until `EAGAIN` to ensure no data is left in the buffer, as `epoll` won't notify again unless new data arrives.
- **Benefit**: Minimizes the number of events returned by `epoll_wait` and reduces kernel-to-user context switches.

### 1.2 SO_REUSEPORT Scalability
- **Strategy**: Run one `epoll` instance per CPU core. Use `SO_REUSEPORT` on the listening socket.
- **Benefit**: The kernel load-balances incoming connections across all reactor threads without a "thundering herd" bottleneck at the accept level.

---

## 2. Multi-Threaded Safety

### 2.1 EPOLLONESHOT
- **Strategy**: For shared-nothing architectures, use `EPOLLONESHOT` to ensure only one thread ever handles a specific FD at a time.
- **Benefit**: Simplifies state management in multi-threaded environments and prevents concurrent execution of the same request handler.
- **Requirement**: Must re-arm the FD after processing with `epoll_ctl(MOD)`.

---

## 3. Ultra-Low Latency Tuning

### 3.1 epoll_pwait2 (Nanosecond Timeout)
- **Strategy**: Use `epoll_pwait2` instead of `epoll_wait`.
- **Benefit**: High-resolution `timespec` timeout (nanoseconds) allows for more precise `MicroTicker` and `TimerWheel` integration.

### 3.2 Busy Wait Dispatch
- **Strategy**: In high-load scenarios, set `timeout = 0` and execute in a loop to bypass the cost of going to sleep.
- **Benefit**: Achieves lowest possible latency by staying in user-space, but consumes 100% CPU.

---

## 4. epoll Implementation Priorities

| Feature | Technique | Priority |
| :--- | :--- | :--- |
| **Edge-Trigger** | `EPOLLET` + Buffer Draining | 🔥 High (v2.4.0) |
| **Scalability** | `SO_REUSEPORT` Multi-Reactor | 🔥 High (v2.4.0) |
| **Safety** | `EPOLLONESHOT` Re-arming | ✅ Medium (v2.4.0) |
| **Precision** | `epoll_pwait2` Nanoseconds | ✅ Medium (v2.5.0) |
