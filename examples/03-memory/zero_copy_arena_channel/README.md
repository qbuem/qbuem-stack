# zero_copy_arena_channel

**Category:** Memory
**File:** `zero_copy_arena_channel_example.cpp`
**Complexity:** Advanced

## Overview

Combines three advanced I/O and memory features in one example:

1. **Zero-copy file transfer** — `sendfile(2)` / `splice(2)` for transferring data between file descriptors without copying to userspace.
2. **ArenaChannel** — a typed channel backed by an `Arena`, so items are allocated in contiguous arena memory instead of the heap.
3. **AsyncLogger** integration — non-blocking structured access logging.

## Scenario

A media server needs to stream large files to clients with minimal CPU overhead (zero-copy), while simultaneously routing processed metadata events through a zero-allocation channel.

## Architecture Diagram

```
  Zero-Copy File Transfer
  ──────────────────────────────────────────────────────────
  File on disk (fd_in)
       │
       │  sendfile(fd_out, fd_in, offset, count)
       │  OR splice(fd_in, NULL, fd_pipe[1], NULL, len, 0)
       │       + splice(fd_pipe[0], NULL, fd_out, NULL, len, 0)
       ▼
  Client socket (fd_out)
  (data never copied into userspace)

  ArenaChannel<T> Flow
  ──────────────────────────────────────────────────────────
  Producer
       │
       │  channel.try_send(item)
       ▼                            Arena storage
  ┌─────────────────────────────────────────────────────┐
  │  ArenaChannel<MetaEvent>                           │
  │  Items live in bump-pointer arena memory           │
  │  Ring of arena-allocated slots                     │
  └────────────────────┬────────────────────────────────┘
                       │  channel.try_recv()
                       ▼
  Consumer (pipeline stage / logger)
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `zero_copy::sendfile(dst_fd, src_fd, offset, len)` | OS-level zero-copy between file and socket FD |
| `zero_copy::splice(src_fd, dst_fd, len)` | Zero-copy between two kernel buffers via pipe |
| `ArenaChannel<T>(arena, capacity)` | Channel backed by arena-allocated ring slots |
| `ArenaChannel::try_send(item)` | Enqueue item using arena allocation |
| `ArenaChannel::try_recv()` | Dequeue item pointer (points into arena) |
| `AsyncLogger` | Non-blocking SPSC access logger |

## Input / Output

### Zero-Copy Transfer

| Input | Output |
|-------|--------|
| Source file FD + byte range | Written to destination FD; 0 bytes copied in userspace |

### ArenaChannel

| Input | Output |
|-------|--------|
| `MetaEvent{id, timestamp, tag}` via `try_send` | Same struct pointer from arena via `try_recv` |

## How to Run

```bash
# Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target zero_copy_arena_channel_example

# Run
./build/examples/03-memory/zero_copy_arena_channel/zero_copy_arena_channel_example
```

## Expected Output

```
=== Zero-Copy Transfer ===
[sendfile] transferred N bytes
[splice]   transferred N bytes via pipe

=== ArenaChannel ===
[arena_ch] sent id=1 tag=frame_0
[arena_ch] sent id=2 tag=frame_1
[arena_ch] recv id=1 tag=frame_0
[arena_ch] recv id=2 tag=frame_1
```

## Notes

- `sendfile` is Linux-specific; the implementation falls back to `read`/`write` on other platforms.
- Items received from `ArenaChannel` are **arena-owned pointers** — do not call `delete` on them; they are freed when the arena is reset.
- Use `ArenaChannel` for hot-path event routing inside a single reactor thread where heap allocation latency is unacceptable.
