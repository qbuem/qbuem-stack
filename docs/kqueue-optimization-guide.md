# qbuem-stack: kqueue Performance Optimization Guide (macOS/BSD)

This guide details the "Extreme Performance" implementation strategies for the `kqueue` reactor on macOS and BSD systems.

## 1. System Call Minimization

### 1.1 kevent Batching (Changelist Optimization)
Unlike `epoll_ctl`, `kqueue` allows multiple registrations in a single call.
- **Strategy**: Implement an `EventBatcher` that collects `EV_ADD`/`EV_DELETE` requests and فلushes them only once per loop iteration via the `changelist` argument of `kevent()`.
- **Benefit**: Significantly reduces user-kernel transitions during high connection churn.

### 1.2 Edge-Triggered Behavior (`EV_CLEAR`)
- **Strategy**: Use the `EV_CLEAR` flag to implement edge-triggered semantics. 
- **Benefit**: Reduces the number of events returned by the kernel when a socket remains ready over multiple loop cycles.

---

## 2. O(1) Event Dispatching

### 2.1 Direct Pointer `udata`
- **Strategy**: Store the address of the `qbuem::Task` or a specialized `IoContext` directly in the `kevent.udata` field.
- **Workflow**:
  1. `kevent.udata = (void*)handler_ptr;`
  2. Upon `kevent` return: `auto* handler = (Handler*)event.udata; handler->resume();`
- **Benefit**: Eliminates `std::unordered_map<fd, handler>` lookups, achieving O(1) dispatch latency.

---

## 3. Real-Time Precision

### 3.1 Nanosecond Timers (`NOTE_NSECONDS`)
- **Strategy**: Use `EVFILT_TIMER` with the `NOTE_NSECONDS` flag for `MicroTicker` implementation on macOS.
- **Benefit**: Provides the highest possible timer resolution available on the XNU kernel.

---

## 4. Zero-Copy & Filesystem

### 4.1 macOS `sendfile`
- **Strategy**: Use `sendfile(fd, s, offset, &len, ...)` for static file serving.
- **Benefit**: Direct transfer from kernel cache to socket without user-space `memcpy`.

### 4.2 APFS Copy Acceleration
- **Strategy**: For `FileSink` to `FileSource` operations on the same volume, use `copyfile(3)` with `COPYFILE_DATA`.
- **Benefit**: Leverages APFS Copy-on-Write (CoW) for instant "cloning" of multi-GB files.

---

## 5. kqueue Implementation Priorities

| Feature | Technique | Priority |
| :--- | :--- | :--- |
| **Dispatch** | `udata` Direct Pointer | 🔥 High (v2.4.0) |
| **Batching** | `kevent` Changelist | 🔥 High (v2.4.0) |
| **Timers** | `NOTE_NSECONDS` | ✅ Medium (v2.4.0) |
| **Zero-Copy** | `sendfile` / `copyfile` | ✅ Medium (v2.6.0) |
