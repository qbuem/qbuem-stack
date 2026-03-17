# kqueue_sophistication

**Category:** Hardware / Platform
**File:** `kqueue_sophistication.cpp`
**Platform:** macOS only
**Complexity:** Advanced

## Overview

Demonstrates advanced `kqueue(2)` usage patterns on macOS: `EVFILT_TIMER` for high-resolution sub-millisecond timers, `EVFILT_USER` for cross-thread signaling, `EVFILT_READ` for non-blocking socket I/O, and batched `kevent64` calls for maximum throughput.

## Scenario

A macOS-native reactor implementation leverages kqueue's advanced filter types beyond basic socket readiness — custom user events for inter-thread coordination, high-precision timers for time-critical tasks, and batch event processing to minimize system call overhead.

## Architecture Diagram

```
  kqueue instance
  ──────────────────────────────────────────────────────────
  ┌─────────────────────────────────────────────────────────┐
  │  kqueue fd                                             │
  │                                                        │
  │  EVFILT_TIMER (ident=1, data=100ms)                    │
  │  └─ fires every 100ms → timer callback                 │
  │                                                        │
  │  EVFILT_USER (ident=42)                                │
  │  └─ signaled via NOTE_TRIGGER from another thread      │
  │     → wake up reactor immediately                      │
  │                                                        │
  │  EVFILT_READ (ident=sockfd)                            │
  │  └─ fires when socket has data to read                 │
  └─────────────────────────────────────────────────────────┘

  Batch kevent64 call:
  struct kevent64_s changes[N];  // register multiple events
  kevent64(kq, changes, N,       // changes to apply
           events, MAX_EVENTS,   // output buffer
           0, NULL)              // flags, timeout
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `kqueue()` | Create kqueue instance |
| `EV_SET64(kev, ident, filter, flags, fflags, data, udata, ext)` | Populate kevent64 struct |
| `EVFILT_TIMER` | Periodic or one-shot timer |
| `EVFILT_USER` | User-space triggered event |
| `EVFILT_READ` | Read readiness filter |
| `NOTE_TRIGGER` | Trigger a `EVFILT_USER` event from another thread |
| `kevent64(kq, changes, nchanges, events, nevents, flags, timeout)` | Register/poll events |

## How to Run

```bash
# macOS only
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target kqueue_sophistication
./build/examples/10-hardware/kqueue_sophistication/kqueue_sophistication
```

## Expected Output

```
[kqueue] registered EVFILT_TIMER (100ms)
[kqueue] registered EVFILT_USER
[kqueue] registered EVFILT_READ (fd=N)
[kqueue] timer fired (t=100ms)
[kqueue] user event triggered from thread
[kqueue] socket readable: N bytes
[kqueue] batch: 3 events processed
```

## Notes

- This example is macOS-exclusive (`#if defined(__APPLE__)`); it will not compile on Linux.
- For Linux equivalents: use `timerfd_create` (timer), `eventfd` (user events), and `epoll` (socket I/O).
- The qbuem-stack kqueue reactor (`KqueueReactor`) uses all these techniques internally.
