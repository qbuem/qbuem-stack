# Windows Support & IOCP Integration

> **Target Version**: v1.8.0-draft
> **Status**: Planning / Architectural Specification

---

## 1. Overview

To truly become a **Universal Distributed Operating Environment**, `qbuem-stack` must provide native support for the Windows platform. Following our **Strict Zero-Dependency** principle, this support is implemented directly using the Win32 API and Winsock2, avoiding mid-layer abstractions like `libuv` or `asio`.

The core challenge lies in bridging the **Readiness-based** interface of our `IReactor` (modeled after `epoll`/`kqueue`) with Windows' **Completion-based** (Proactor) `IOCP` model.

---

## 2. Technical Architecture: Bridging Readiness and Completion

### 2.1 IOCP (I/O Completion Ports)
Windows uses `IOCP`, where the kernel performs the operation and notifies the application upon completion.

### 2.2 Bridging via "Zero-Byte Peek"
To maintain our current `Reactor::register_event(fd, Type, Callback)` contract without a massive rewrite:
- **Read Readiness**: Issue a `WSARecv` with a 0-byte buffer.
- **Completion**: When the 0-byte recv completes, the socket is "ready" for a real read.
- **Dispatch**: Trigger the `read_cb` just like a `kqueue` or `epoll` event.

---

## 3. Handle Abstraction

Windows uses `HANDLE` and `SOCKET` types, which are pointers or unsigned integers, unlike POSIX's simple `int`.

### `qbuem::Fd` Type
We will introduce a cross-platform handle type:
```cpp
#ifdef _WIN32
  using Fd = uintptr_t; // SOCKET/HANDLE
#else
  using Fd = int;        // POSIX fd
#endif
```

---

## 4. Zero-Dependency Foundations

| Component | Windows Implementation |
| :--- | :--- |
| **Networking** | `Winsock2.h` (WSAStartup, WSASocket, etc.) |
| **Event Loop** | `GetQueuedCompletionStatusEx` (IOCP) |
| **Wakeup** | `PostQueuedCompletionStatus` |
| **Zero-Copy** | `TransmitFile` (equivalent to `sendfile`) |
| **Timers** | `CreateTimerQueueTimer` or shared `TimerWheel` |

---

## 5. Implementation Roadmap

### Phase 1: Foundation (v1.8.0)
- Unified `Fd` type and cross-platform socket primitives.
- `WinReactor` implementation via `GetQueuedCompletionStatusEx`.
- `IOCPEntry` pool management using `FixedPoolResource`.

### Phase 2: Performance (v1.8.5)
- Support for `ConnectEx`, `AcceptEx`, and `DisconnectEx`.
- Integration with Windows Named Pipes for low-latency IPC.
- Overlapped I/O for `AsyncFile` support.

---

## 6. Strategic Benefits

- **Universal Edge**: Industrial IoT and embedded Windows devices.
- **Native Performance**: Reaching the theoretical limits of the Windows kernel.
- **No WSL Required**: Direct development on Windows workstations with full performance.

---

*qbuem-stack — Engineering for total platform universality.*
