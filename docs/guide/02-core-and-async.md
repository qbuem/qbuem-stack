# Core & Async Runtime

The `include/qbuem/core/` headers are the heart of qbuem-stack: the coroutine
type (`Task<T>`), the single-threaded event loop (`Reactor` + its three platform
backends), the multi-core orchestrator (`Dispatcher`), the I/O awaiters, the
zero-allocation memory primitives (`Arena`, `FixedPoolResource`), the
hierarchical `TimerWheel`, the lock-free `AsyncLogger`, CPU mechanical-sympathy
hints, the sub-millisecond `MicroTicker`, NUMA/huge-page utilities, and the two
service-layer interfaces `ISessionStore` and `ITransport`.

Everything here is **header-centric, zero-dependency** (C++23 stdlib + OS/arch
intrinsics only) and follows the four pillars: **Zero Latency, Zero Copy, Zero
Allocation, Zero Dependency**. Errors are returned, never thrown — almost every
fallible call returns `Result<T>` (= `std::expected<T, std::error_code>`) or
`Task<Result<T>>`.

> All symbols live in namespace `qbuem`. `Result<T>`, `unexpected<E>`,
> `BufferView`, and `MutableBufferView` come from `<qbuem/common.hpp>`:
> ```cpp
> template <typename T> using Result = std::expected<T, std::error_code>;
> template <typename E> using unexpected = std::unexpected<E>;
> using BufferView        = std::span<const uint8_t>;
> using MutableBufferView = std::span<uint8_t>;
> ```

---

## Mental model: the reactor + coroutine architecture

qbuem-stack is a **shared-nothing, single-thread-per-reactor** runtime, the same
pattern used by Nginx, Redis, and Node.js.

```
                    ┌──────────── Dispatcher ────────────┐
                    │  one std::jthread per CPU core      │
                    │                                     │
   listen fd  ──►   │  ┌─Reactor 0─┐  ┌─Reactor 1─┐  ...  │
                    │  │ epoll /   │  │ epoll /   │       │
                    │  │ io_uring/ │  │ io_uring/ │       │
                    │  │ kqueue    │  │ kqueue    │       │
                    │  │ poll()    │  │ poll()    │       │
                    │  └───────────┘  └───────────┘       │
                    └─────────────────────────────────────┘

   coroutine (Task<T>)  ──co_await AsyncRead/Write/Sleep/Accept──►
       suspends, registers an event on Reactor::current(),
       resumes on the SAME reactor thread when the event fires
```

Two rules dominate everything below:

1. **A `Reactor` runs on exactly one thread.** All allocators, the
   `TimerWheel`, and the `Task` machinery are intentionally **not thread-safe** —
   each reactor thread owns its own. To touch a reactor from another thread, use
   `Reactor::post()` / `Dispatcher::post()`.
2. **Never call `handle.resume()` across threads.** To wake a coroutine living
   on another reactor, marshal it with that reactor's `post()`.

| Platform | Default reactor backend | Notes |
|---|---|---|
| Linux x86_64 / ARM64 (Jetson-class) | `IOUringReactor` or `EpollReactor` | io_uring needs kernel ≥ 5.x; epoll is the universal Linux fallback |
| Mac aarch64 | `KqueueReactor` | uses `EVFILT_READ/WRITE/USER/TIMER` + an internal `TimerWheel` |

There is **no `create_reactor()` factory** — instantiate the platform reactor
directly (see the `#if defined(__linux__)` pattern in
`examples/01-foundation/micro_ticker/`), or let `Dispatcher` create one per core
for you.

---

## `Task<T>` — the coroutine type

`<qbuem/core/task.hpp>`

### What it is / role
`Task<T>` is a **single-continuation, lazily-started** coroutine type with
**symmetric transfer**. A coroutine whose return type is `Task<T>` (or
`Task<Result<T>>`, or `Task<void>`) can `co_await` other tasks/awaiters and
`co_return` a value. `await_suspend` returns the awaited coroutine's handle so
the compiler tail-calls into it (no stack growth on deep `co_await` chains).

### When to use it
- The return type of every async function in the codebase: route handlers,
  pipeline stage functions, transport reads, DB queries.
- Fire-and-forget background work via `detach()` / `Dispatcher::spawn()`.

When **not** to: do not use `Task<T>` for a synchronous pure-CPU helper — plain
functions returning `Result<T>` are cheaper (no coroutine frame). Use
`Task<Result<T>>` only when the function actually suspends on I/O or a timer.

### Key surface

| Member | Signature | Notes |
|---|---|---|
| `value_type` | `= T` | the success type |
| `resume()` | `bool resume()` (`Task<void>`: `[[nodiscard]] bool resume() const`) | drive a top-level task one step; returns `true` if still suspended |
| `detach()` | `void detach()` | release ownership — frame self-destructs on completion (fire-and-forget) |
| `await_ready / await_suspend / await_resume` | awaiter interface | lets one `Task` `co_await` another |
| `handle` | `std::coroutine_handle<promise_type>` | the raw frame (advanced use) |

`Task` is **move-only** (copy is deleted). The destructor calls `handle.destroy()`
unless the task was `detach()`-ed.

### How to use it
A stage function — the mandatory signature used across the pipeline and HTTP
layers — returns `Task<Result<T>>` and propagates errors as values:

```cpp
#include <qbuem/core/task.hpp>
#include <qbuem/common.hpp>
using namespace qbuem;

Task<Result<int>> parse_port(std::string_view s, std::stop_token st) {
  if (st.stop_requested())
    co_return unexpected(std::make_error_code(std::errc::operation_canceled));

  int port = 0;
  for (char c : s) {
    if (c < '0' || c > '9')
      co_return unexpected(std::make_error_code(std::errc::invalid_argument));
    port = port * 10 + (c - '0');
  }
  co_return port;                       // implicit success
}

Task<Result<void>> run() {
  auto r = co_await parse_port("8080", {});   // co_await another Task
  if (!r) {
    // error path — r.error() is a std::error_code
    co_return unexpected(r.error());
  }
  // use *r ...
  co_return Result<void>{};             // void success
}
```

Fire-and-forget via `detach()` (or, more commonly, `Dispatcher::spawn()`):

```cpp
Task<void> background_job() {
  co_await sleep(50);                    // AsyncSleep awaiter
  // ... work that nobody awaits ...
  co_return;
}

void kickoff() {
  Task<void> t = background_job();
  t.detach();                            // frame now self-owns; do NOT touch t.handle after this
}
```

### Gotchas / constraints
- **No exceptions (Pillar — A9/M1).** The promise's `unhandled_exception()`
  calls `std::terminate()` unless you install a global handler via
  `qbuem::set_unhandled_exception_handler(...)`. If a handler is installed and a
  `Task<T>` (non-void) finished via the exception path, `await_resume()` finds an
  empty `std::optional` and calls `std::terminate()` deterministically — so the
  no-throw contract is effectively mandatory. **Always model failure with
  `Result<T>` / `co_return unexpected(...)`.**
- **Not thread-safe (lifetime).** `detach()`, `final_suspend()`, and
  `await_suspend()` all touch the promise. They must all happen on the **same
  thread** (the owning reactor). Crossing threads is a TSan data race; wake other
  reactors via `post()` instead.
- **Lazy start.** `initial_suspend()` is `std::suspend_always`: a freshly
  created `Task` does nothing until `resume()`-ed or `co_await`-ed. Top-level
  tasks (not awaited by anyone) must be driven by `resume()` or handed to
  `Dispatcher::spawn()`.
- The promise overrides `operator new`/`delete` to force a heap frame, which
  prevents a GCC HALO mis-optimization that caused stack-use-after-return under
  ASan. This is deliberate; do not "optimize" it away.

---

## `Reactor` — the abstract event loop

`<qbuem/core/reactor.hpp>`

### What it is / role
`Reactor` is the platform-independent abstract base class for the single-threaded
event loop. Coroutine awaiters and the `Dispatcher` interact with the world
exclusively through this interface; the concrete backend (`epoll`, `io_uring`,
`kqueue`) is chosen at the call site.

### When to use it
- You almost never call `register_event` directly — the awaiters do it for you.
  You **do** use `Reactor::current()` inside coroutines, `reactor->poll(0)` when
  driving a reactor from a `MicroTicker`, and `reactor->post(fn)` to safely
  schedule work onto a reactor thread.
- When **not** to: for ordinary servers, let `Dispatcher` own the reactors and
  use `App::listen()` / `Dispatcher::spawn()`. Hand-driving a `poll()` loop is
  only for specialist real-time loops.

### Event types
```cpp
enum class EventType { Read, Write, Error };
```

### Interface catalogue

| Method | Signature | Purpose |
|---|---|---|
| `register_event` | `Result<void> register_event(int fd, EventType, std::function<void(int)> cb)` | watch an fd; `cb(fd)` fires on readiness. fd **must** be `O_NONBLOCK` |
| `unregister_event` | `Result<void> unregister_event(int fd, EventType)` | stop watching (awaiters call this after each event) |
| `register_timer` | `Result<int> register_timer(int timeout_ms, std::function<void(int)> cb)` | one-shot timer; returns a timer id, `cb(timer_id)` on expiry |
| `unregister_timer` | `Result<void> unregister_timer(int timer_id)` | cancel an unfired timer |
| `register_signal` / `unregister_signal` | `Result<void> register_signal(int sig, std::function<void(int)>)` / `Result<void> unregister_signal(int sig)` | POSIX signal handling on the loop |
| `register_write_timeout` | `Result<int> register_write_timeout(int fd, int timeout_ms, std::function<void(int)> cb)` | non-virtual helper: arms a timer that unregisters the Write event + calls `cb(fd)` on deadline |
| `poll` | `Result<int> poll(int timeout_ms)` | run one loop iteration (`-1` = block until an event); returns #events |
| `stop` | `void stop()` | stop the loop; thread-safe, callable from another thread |
| `is_running` | `bool is_running() const` | loop state |
| `post` | `void post(std::function<void()> fn)` | **thread-safe** — enqueue `fn` to run on the reactor thread |
| `current` (static) | `static Reactor *current()` | the calling thread's reactor (thread-local), or `nullptr` |
| `set_current` (static) | `static void set_current(Reactor *r)` | called by `Dispatcher` per worker; rarely called by you |

### How to use it
Driving a reactor directly (the canonical real-time pattern, from
`examples/01-foundation/micro_ticker/`):

```cpp
#include <qbuem/core/micro_ticker.hpp>
#if defined(__linux__)
#  include <qbuem/core/epoll_reactor.hpp>
   using PlatformReactor = qbuem::EpollReactor;
#else
#  include <qbuem/core/kqueue_reactor.hpp>
   using PlatformReactor = qbuem::KqueueReactor;
#endif
using namespace qbuem;

auto reactor = std::make_unique<PlatformReactor>();

reactor->register_timer(10, [](int /*timer_id*/) {
  // fires once, ~10 ms later
});

MicroTicker ticker(std::chrono::microseconds{100});
ticker.run([&](uint64_t tick) {
  reactor->poll(0);          // non-blocking: fire any ready callbacks
  // ... your per-tick logic ...
  if (tick + 1 >= 200) ticker.stop();
});
```

Safely waking a reactor from another thread:

```cpp
// On any thread:
reactor->post([] {
  // runs inside the reactor's poll() loop, on its thread
});
```

### Gotchas / constraints
- **`O_NONBLOCK` is mandatory** on any fd you register. A blocking fd can stall
  the whole loop (Pillar L1).
- Everything except `stop()` and `post()` must be called **on the reactor's own
  thread**.
- `poll(timeout_ms)` should keep the timeout small (≤ 1 ms is the pillar guidance,
  L6) for reactors that also drive timers; `poll(0)` is the non-blocking form used
  under a `MicroTicker`.
- `register_event` on the **same** `{fd, type}` overwrites the previous callback.

---

## Concrete reactors: `EpollReactor`, `IOUringReactor`, `KqueueReactor`

### Platform selection & graceful fallback

| Backend | Header | Platform | Key behaviour |
|---|---|---|---|
| `EpollReactor` | `<qbuem/core/epoll_reactor.hpp>` | Linux | edge-triggered (`EPOLLET | EPOLLONESHOT`), `timerfd` timers, `eventfd` wakeup for `post()` |
| `IOUringReactor` | `<qbuem/core/io_uring_reactor.hpp>` | Linux ≥ 5.x | `POLL_ADD` readiness + `IORING_OP_TIMEOUT`; optional fixed buffers & buffer rings; SQPOLL when permitted |
| `KqueueReactor` | `<qbuem/core/kqueue_reactor.hpp>` | macOS / BSD | `EVFILT_READ/WRITE/USER/TIMER`, batched `kevent`, internal `TimerWheel`, `FixedPoolResource`-backed entries |

The pattern for portable code is a compile-time `using` alias (see the
`#if defined(__linux__)` block above). The choice between `io_uring` and `epoll`
on Linux is an application decision — `epoll` is the universal fallback when
io_uring is unavailable or undesirable.

### `EpollReactor` — Linux default
Edge-triggered + one-shot semantics mean **callbacks must drain to `EAGAIN`** (no
re-notification until new data arrives). `EPOLLONESHOT` auto-disarms the fd after
each event and the reactor re-arms it via `EPOLL_CTL_MOD`, so at most one thread
processes a given fd at a time — safe under a multi-threaded `Dispatcher`. Use
`SO_REUSEPORT` on listeners to spread connections across one reactor per core
without an accept thundering-herd.

```cpp
#include <qbuem/core/epoll_reactor.hpp>
using namespace qbuem;

EpollReactor reactor;
// fd is O_NONBLOCK; drain until EAGAIN inside the callback:
auto r = reactor.register_event(fd, EventType::Read, [&](int f) {
  for (;;) {
    char buf[4096];
    ssize_t n = ::read(f, buf, sizeof(buf));
    if (n <= 0) break;        // EAGAIN / EOF — stop, wait for next event
    // ... consume buf[0..n) ...
  }
});
if (!r) { /* r.error() */ }

while (reactor.is_running())
  reactor.poll(1);            // 1 ms cap
```

### `IOUringReactor` — Linux high-end (Jetson-class & servers)
Beyond the `Reactor` interface, `IOUringReactor` exposes zero-copy DMA paths.
`QUEUE_DEPTH` is `256`. `is_sqpoll()` reports whether the kernel accepted
`IORING_SETUP_SQPOLL` (falls back to normal mode without it).

| Extra method | Signature |
|---|---|
| `is_sqpoll` | `bool is_sqpoll() const noexcept` |
| `register_fixed_buffers` | `Result<void> register_fixed_buffers(std::span<const iovec>)` |
| `unregister_fixed_buffers` | `void unregister_fixed_buffers() noexcept` |
| `fixed_buffer_count` | `size_t fixed_buffer_count() const noexcept` |
| `read_fixed` | `Result<void> read_fixed(int fd, int buf_idx, std::span<std::byte> buf, int64_t file_offset, std::function<void(int)> cb)` |
| `write_fixed` | `Result<void> write_fixed(int fd, int buf_idx, std::span<const std::byte> buf, int64_t file_offset, std::function<void(int)> cb)` |
| `register_buf_ring` | `Result<void> register_buf_ring(uint16_t bgid, size_t buf_size, size_t buf_count)` (Linux 5.19+ / liburing 2.2+) |
| `unregister_buf_ring` | `void unregister_buf_ring(uint16_t bgid) noexcept` |
| `recv_buffered` | `Result<void> recv_buffered(int fd, uint16_t bgid, std::function<void(int, uint16_t, void*)> cb)` |
| `return_buf_to_ring` | `void return_buf_to_ring(uint16_t bgid, uint16_t buf_id) noexcept` |

```cpp
#include <qbuem/core/io_uring_reactor.hpp>
#include <sys/uio.h>
using namespace qbuem;

IOUringReactor reactor;

// Register a pinned buffer for zero-copy DMA reads:
alignas(4096) static std::byte dma[64 * 1024];
iovec iov{ dma, sizeof(dma) };
if (auto r = reactor.register_fixed_buffers(std::span{&iov, 1}); !r) {
  // r.error()
}

// IORING_OP_READ_FIXED — no copy through user space:
reactor.read_fixed(fd, /*buf_idx=*/0, std::span<std::byte>{dma, 4096},
                   /*file_offset=*/-1,            // -1 = streaming fd
                   [](int res) {
                     if (res < 0) { /* errno = -res */ }
                     else { /* res bytes in dma[0..res) */ }
                   });
```

**Constraint:** the `iovec` array passed to `register_fixed_buffers` must outlive
the registration (until `unregister_fixed_buffers()`); buffers passed to
`read_fixed/write_fixed` must lie within the registered range. For buffer rings,
you **must** call `return_buf_to_ring(bgid, buf_id)` after consuming each
`recv_buffered` buffer or the pool is exhausted.

### `KqueueReactor` — macOS / Mac aarch64
The macOS backend uses `kqueue` with batched change/event lists. Internally it
allocates per-fd entries from a `FixedPoolResource` and manages timers through a
`TimerWheel` (so the macOS timer path is the same O(1) wheel described below).
Same `Reactor` interface; instantiate it directly or via the portable alias.

```cpp
#include <qbuem/core/kqueue_reactor.hpp>
using namespace qbuem;

KqueueReactor reactor;
reactor.register_timer(100, [](int) { /* ~100 ms later */ });
while (reactor.is_running()) reactor.poll(1);
```

---

## `Dispatcher` — multi-core orchestrator

`<qbuem/core/dispatcher.hpp>`

### What it is / role
`Dispatcher` creates one `Reactor` per CPU core (by default
`std::thread::hardware_concurrency()`), runs each on its own `std::jthread`, and
distributes work. It is the **concurrency entry point**: most apps create exactly
one and call `run()` at the end of `main()`.

### When to use it
- Any server / multi-connection workload. With `SO_REUSEPORT` you give each
  reactor its own listening socket via `register_listener_at`, so accepts scale
  linearly across cores.
- When **not** to: a single dedicated real-time loop (one core, one
  `MicroTicker` driving one reactor) does not need a `Dispatcher`.

### Surface catalogue

| Method | Signature | Purpose |
|---|---|---|
| ctor | `explicit Dispatcher(size_t thread_count = hardware_concurrency())` | creates reactors now; threads start at `run()`. `0` → at least 1 thread |
| `run` | `void run()` | start all workers; **blocks** until `stop()` / all threads finish |
| `stop` | `void stop()` | stop all reactors; thread-safe |
| `register_listener` | `Result<void> register_listener(int fd, std::function<void(int)> cb)` | assign fd to a worker by `fd % thread_count` (sticky) |
| `register_listener_at` | `Result<void> register_listener_at(int fd, size_t reactor_idx, std::function<void(int)> cb)` | pin fd to a specific worker (SO_REUSEPORT pattern) |
| `get_worker_reactor` | `Reactor* get_worker_reactor(int fd)` | the reactor that owns this fd (or `nullptr`) |
| `thread_count` | `size_t thread_count() const noexcept` | number of workers/reactors |
| `post` | `void post(std::function<void()> fn)` | round-robin a callback onto some worker |
| `post_to` | `void post_to(size_t reactor_idx, std::function<void()> fn)` | callback onto a specific worker |
| `spawn` | `void spawn(Task<void>)` / `void spawn(Task<Result<void>>)` | fire-and-forget a coroutine round-robin; ownership transferred (`detach`-ed internally). The `Result<void>` overload discards errors |
| `spawn_on` | `void spawn_on(size_t reactor_idx, Task<void>)` | fire-and-forget on a specific worker |

`Dispatcher` is neither copyable nor movable.

### How to use it
```cpp
#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
using namespace qbuem;

Task<void> worker_job() {
  co_await sleep(10);     // runs on whichever reactor it landed on
  co_return;
}

int main() {
  Dispatcher dispatcher;                  // one reactor per core

  // SO_REUSEPORT: one listening socket per reactor, pinned per worker
  for (size_t i = 0; i < dispatcher.thread_count(); ++i) {
    int lfd = make_reuseport_listener(8080);   // your helper; sets O_NONBLOCK
    auto r = dispatcher.register_listener_at(lfd, i, [](int fd) {
      // accept on this reactor's thread
    });
    if (!r) return 1;                     // r.error().message()
  }

  dispatcher.spawn(worker_job());         // fire-and-forget
  dispatcher.run();                       // blocks
}
```

### Gotchas / constraints
- `run()` blocks; call `stop()` from a signal handler or another thread to
  unblock (see `examples/03-memory/numa_hugepages/`, which runs `disp.run()` in a
  `std::jthread`).
- `spawn`/`spawn_on` **transfer ownership** of the `Task` and detach it; do not
  use the task object afterward. The `Task<Result<void>>` overload silently
  swallows errors — wrap your own logging if you need it.
- A given fd is always handled by the same reactor for its whole lifetime
  (sticky), which keeps per-connection state thread-confined.

### Graceful shutdown: `in_flight()` + `drain()`
`stop()` halts the reactors immediately and abandons any suspended coroutine
frames. For an orderly shutdown, stop accepting new work (close your listeners),
then call `drain(timeout)`: it waits for `in_flight()` (the count of spawned
coroutines that have not yet completed) to reach 0, then calls `stop()`. It
returns the number still outstanding — `0` means a clean drain, `>0` means the
timeout forced shutdown with work in flight. Call it from a non-reactor thread
(same constraint as `stop()`).

---

## `OffloadPool` — blocking / CPU-bound work off the reactor

### What it is / role
A fixed-size worker-thread pool (`<qbuem/core/offload_pool.hpp>`) for the work
that must NOT run on a reactor thread: synchronous third-party libraries,
CPU-bound transforms (compression, large-buffer hashing, media encoding), or
file I/O on platforms without io_uring. It is the sanctioned escape hatch from
the "no blocking on a reactor thread" rule (L1).

### How to use it
```cpp
#include <qbuem/core/offload_pool.hpp>

OffloadPool pool;   // create once; share across the app (default: hw concurrency)

Task<Result<void>> handle(Request req, std::stop_token) {
    // Runs on a pool thread; the coroutine resumes on its ORIGIN reactor.
    auto digest = co_await pool.run([body = std::move(req).body()] {
        return expensive_blocking_hash(body);   // safe: off the reactor
    });
    co_return Response{}.body(digest);
}
```
`co_await pool.run(fn)` captures the current reactor, runs `fn` on a pool worker,
then posts the resume back to that reactor (no cross-reactor resume), and yields
`fn()`'s return value. Use `submit(fn)` for fire-and-forget work with no result.

### Gotchas / constraints
- Await it from a reactor coroutine so resume affinity is preserved; off a
  reactor (e.g. a bare unit test) it resumes inline on the pool thread.
- The pool must outlive every in-flight `run()`. Shut it down only after the
  reactors that own those coroutines have drained; `shutdown()` runs all
  already-queued jobs before joining, so pending resumes are still posted.
- The queue uses a mutex/condvar — fine here, because the whole point is to keep
  this (non-latency-critical) work OFF the latency-critical reactor threads.
- Runnable reference: `examples/01-foundation/offload_pool/`.

---

## I/O awaiters: `AsyncRead`, `AsyncWrite`, `AsyncSleep`, `AsyncAccept`

`<qbuem/core/awaiters.hpp>`

### What they are / role
Tiny `co_await`-able structs that suspend the current coroutine, register the
appropriate event on `Reactor::current()`, and resume on completion. They are the
ergonomic bridge between the reactor and `Task<T>` coroutines.

| Awaiter | Fields | `co_await` yields |
|---|---|---|
| `AsyncRead` | `int fd; void* buf; size_t count;` | `ssize_t` — `::read` result (`-1` on error) |
| `AsyncWrite` | `int fd; const void* buf; size_t count;` | `ssize_t` — `::write` result |
| `AsyncSleep` | `int timeout_ms;` | `void` (ready immediately if `timeout_ms <= 0`) |
| `AsyncAccept` | `int listen_fd;` | `int` — accepted client fd (`-1` on error) |

Free helper: `inline AsyncSleep sleep(int ms)`.

### How to use it
Async sleep inside a handler (from `examples/01-foundation/async_timer/`):

```cpp
#include <qbuem/core/awaiters.hpp>
using namespace qbuem;

app.get("/sleep", AsyncHandler([](const Request&, Response& res) -> Task<void> {
  co_await sleep(1000);                 // 1 s non-blocking delay
  res.status(200).body("Hello after 1s sleep!");
  co_return;
}));
```

An accept loop (the documented `AsyncAccept` pattern):

```cpp
Task<void> accept_loop(int listen_fd) {
  while (true) {
    int client_fd = co_await AsyncAccept{listen_fd};
    if (client_fd < 0) break;
    // set O_NONBLOCK on client_fd, then handle it ...
  }
  co_return;
}
```

Reading bytes:

```cpp
Task<void> echo_once(int fd) {
  char buf[4096];
  ssize_t n = co_await AsyncRead{fd, buf, sizeof(buf)};
  if (n > 0)
    co_await AsyncWrite{fd, buf, static_cast<size_t>(n)};
  co_return;
}
```

### Gotchas / constraints
- They require `Reactor::current() != nullptr`. If no reactor is set on the
  thread, the awaiter degrades to **immediately resuming without doing the I/O**
  (the read/write never happens and `result` stays `-1`, sleep returns instantly).
  Always run these on a reactor thread (inside a `Dispatcher` worker or a
  hand-driven `poll()` loop).
- Each awaiter `unregister_event`s itself after firing, so a single `co_await`
  is one-shot — loop for repeated I/O (as in the accept loop).
- `AsyncRead`/`AsyncWrite` issue a single `::read`/`::write`; for edge-triggered
  `EpollReactor` you typically drain in a loop. For high-throughput scatter-gather,
  prefer the io_uring fixed-buffer path or the `io/` scatter-gather facilities
  rather than single-buffer awaiters (Pillar C3).

---

## Zero-allocation memory model: `Arena` & `FixedPoolResource`

`<qbuem/core/arena.hpp>`

These two allocators implement Pillar 3 (Zero Allocation) for the hot path. Both
are **single-threaded by design** — one per reactor thread, no locks.

### `Arena` — bump-pointer allocator

**What / role:** O(1) bump allocation for variable-size, short-lived objects with
a shared lifetime (e.g. everything created while handling one HTTP request).
`reset()` reclaims everything in O(1) without freeing to the OS.

**When to use:** per-request / per-message scratch memory. Allocate during the
request, `reset()` at the end, reuse the same blocks next request.
**When not to:** long-lived objects, or objects needing individual `free` — Arena
has no per-object deallocation. Objects needing destruction must be destroyed
explicitly (Arena does not run destructors).

| Member | Signature |
|---|---|
| ctor | `explicit Arena(size_t initial_size = 64 * 1024)` |
| `allocate` | `void* allocate(size_t size, size_t alignment = alignof(std::max_align_t))` |
| `reset` | `void reset()` (O(1); keeps blocks for reuse) |
| `block_count` | `size_t block_count() const noexcept` (introspection) |

Move-only. `allocate` never returns `nullptr`; it throws `std::bad_alloc` only if
the OS is out of memory (a cold-path event, not a hot-path concern).

```cpp
#include <qbuem/core/arena.hpp>
using namespace qbuem;

struct HttpHeader { char name[64]; char value[128]; };

Arena arena(4096);
auto* h = static_cast<HttpHeader*>(
    arena.allocate(sizeof(HttpHeader), alignof(HttpHeader)));   // O(1)
std::strncpy(h->name, "Content-Type", sizeof(h->name) - 1);
// ... handle request using arena-allocated objects ...
arena.reset();    // O(1) — all arena pointers now invalid; blocks reused
```

**Gotcha:** after `reset()` (or destruction) **every** pointer previously handed
out is dangling. The arena reuses already-allocated trailing blocks across
`reset()` cycles, so `block_count()` stabilizes instead of growing unbounded.
See `examples/03-memory/arena/`.

### `FixedPoolResource<ObjectSize, Alignment>` — free-list pool

**What / role:** O(1) allocate/deallocate of **same-size** slots, with the
free-list pointer embedded in each free slot (no side metadata). Backs hot,
fixed-size object churn: connections, coroutine-adjacent contexts, timer entries.

**When to use:** repeated alloc/free of identical-size objects.
**When not to:** variable sizes (use `Arena`), or cross-thread sharing (provide
your own lock, or use one pool per thread).

```cpp
template <size_t ObjectSize, size_t Alignment = alignof(std::max_align_t)>
class FixedPoolResource;
```

| Member | Signature | Notes |
|---|---|---|
| ctor | `explicit FixedPoolResource(size_t capacity)` | one upfront aligned allocation of `capacity * slot` |
| `allocate` | `[[nodiscard]] void* allocate() noexcept` | O(1); returns `nullptr` when exhausted |
| `deallocate` | `void deallocate(void* ptr) noexcept` | O(1); prepends to free-list |
| `capacity` / `used` / `available` | `size_t … const noexcept` | introspection |

Compile-time invariant: `ObjectSize >= sizeof(void*)` (static_assert) because the
slot stores the free-list link.

```cpp
#include <qbuem/core/arena.hpp>
using namespace qbuem;

struct Connection { int fd; char peer[64]; size_t bytes_read; };

FixedPoolResource<sizeof(Connection), alignof(Connection)> pool(32);

void* slot = pool.allocate();                  // O(1); nullptr if full
if (slot) {
  auto* c = new (slot) Connection{};           // placement new — pool gives raw memory
  c->fd = 10;
  // ... use c ...
  c->~Connection();                            // explicit dtor (pool won't call it)
  pool.deallocate(slot);                       // O(1)
}
```

**Gotchas:** the pool returns **uninitialized** memory — construct with placement
`new` and destroy explicitly before `deallocate`. Passing a foreign pointer (or
`nullptr`) to `deallocate` is UB. Not thread-safe. See
`examples/03-memory/arena/` (pool overflow → `nullptr` is demonstrated there).

---

## `TimerWheel` — O(1) hierarchical timers

`<qbuem/core/timer_wheel.hpp>`

### What it is / role
A 4-level × 256-slot hierarchical timing wheel: O(1) `schedule`, O(1)-amortized
`cancel`, `tick(elapsed)` fires expired callbacks. Entry objects come from an
internal `FixedPoolResource` (no heap churn). It is the high-performance backend
that `KqueueReactor` uses internally, and you can drive it yourself in a custom
loop.

### Resolution & complexity
| Level | Granularity | Max delay |
|---|---|---|
| 0 | 1 ms | 256 ms |
| 1 | 256 ms | ~65.5 s |
| 2 | 65.536 s | ~4.7 h |
| 3 | ~4.7 h | ~49.7 days |

| Member | Signature | Notes |
|---|---|---|
| types | `using Callback = std::function<void()>; using TimerId = uint64_t;` | `kInvalid = 0` sentinel |
| ctor | `explicit TimerWheel(size_t pool_capacity = 4096)` | max simultaneous timers |
| `schedule` | `[[nodiscard]] TimerId schedule(uint64_t delay_ms, Callback)` | `kInvalid` if pool exhausted |
| `cancel` | `bool cancel(TimerId)` | `true` if cancelled before firing |
| `tick` | `size_t tick(uint64_t elapsed_ms)` | advances clock, returns #fired |
| `next_expiry_ms` | `[[nodiscard]] uint64_t next_expiry_ms() const noexcept` | `UINT64_MAX` if none; use as poll timeout |
| `now_ms` / `count` | `uint64_t now_ms() const`, `size_t count() const` | introspection |

`TimerWheel` is neither copyable nor movable (intrusive linked lists).

### How to use it (driving a reactor loop)
```cpp
#include <qbuem/core/timer_wheel.hpp>
using namespace qbuem;

TimerWheel wheel;                                   // 4096-entry pool
auto id = wheel.schedule(100, [] { /* 100 ms elapsed */ });

// In your event loop:
uint64_t timeout = wheel.next_expiry_ms();          // UINT64_MAX if idle
int ms = (int)std::min<uint64_t>(timeout, INT_MAX);
// ... epoll_wait(epfd, ev, N, ms); or reactor.poll(ms); ...
wheel.tick(elapsed_since_last_tick_ms);             // fires due callbacks

wheel.cancel(id);                                   // false if already fired
```

### Gotchas / constraints
- **Single-threaded only.** `schedule/cancel/tick/next_expiry_ms` must all run on
  the same (reactor) thread; concurrent access is a TSan race.
- `schedule` returns `kInvalid` (0) when the `FixedPoolResource` pool is
  exhausted — size `pool_capacity` above your peak concurrent timer count.
- `next_expiry_ms()` scans the index, so prefer it for moderate timer counts; for
  the reactor backends this cost is amortized by the loop structure.

---

## `AsyncLogger` — lock-free access logger

`<qbuem/core/async_logger.hpp>`

### What it is / role
A lock-free ring-buffer logger. The reactor (hot path) `log(...)`s without
blocking or allocating; a background `std::jthread` drains the ring and formats
to a `FILE*` (default `stderr`) as Text or JSON. Fixed-size `LogEntry` (8-byte
method, 256-byte path) keeps the hot path allocation-free.

### When to use it
Access/request logging from reactor threads where you must not stall on I/O or
`std::format` (Pillar L7/A5). Plug it into `App::set_access_logger()` via
`make_callback()`.
**When not to:** general structured app logging with arbitrary fields — `LogEntry`
is purpose-built for `method/path/status/duration`.

| Member | Signature |
|---|---|
| `LogFormat` | `enum class LogFormat { Text, Json }` |
| ctor | `explicit AsyncLogger(size_t capacity = 4096, FILE* out = stderr, LogFormat = LogFormat::Text)` — `capacity` **must be a power of two** |
| `start` / `stop` | `void start()` (spawn flush thread) / `void stop()` (drain + join) |
| `log` | `void log(std::string_view method, std::string_view path, int status, long duration_us) noexcept` |
| `make_callback` | `std::function<void(std::string_view, std::string_view, int, long)> make_callback()` |

Non-copyable; the destructor calls `stop()`.

```cpp
#include <qbuem/core/async_logger.hpp>
using namespace qbuem;

AsyncLogger logger(1024, stderr, LogFormat::Json);   // power-of-two capacity
logger.start();

logger.log("GET", "/api/users", 200, 1234);          // hot path: no alloc, no block

// Integrate with the HTTP App:
// app.set_access_logger(logger.make_callback());     // logger must outlive app

logger.stop();                                        // flush remaining + join
```

### Gotchas / constraints
- **Capacity must be a power of two** (the ring masks with `capacity - 1`); a
  non-power-of-two silently misbehaves.
- When the buffer is full, entries are **silently dropped** — this is deliberate,
  to never block the hot path.
- `make_callback()` captures `this`; the `AsyncLogger` **must outlive** the `App`
  (or any holder of the callback).
- Despite the SPSC-style design, `log()` is guarded for multiple producers via a
  spin `atomic_flag`, so several reactor threads may share one logger — though one
  logger per reactor is the lowest-contention choice.

---

## `cpu_hints.hpp` — mechanical sympathy

`<qbuem/core/cpu_hints.hpp>`

### What it is / role
Prefetch hints, cache-line constants, branch hints, and a portable `cpu_pause()`
for spin loops — Pillar 5 (Hardware Alignment) helpers.

| Symbol | Signature / value | Purpose |
|---|---|---|
| `kCacheLineSize` | `inline constexpr size_t` (= `std::hardware_destructive_interference_size`, else 64) | `alignas(kCacheLineSize)` to avoid false sharing |
| `prefetch_read<Locality=3>` | `void prefetch_read(const void*) noexcept` | `__builtin_prefetch(p,0,L)` |
| `prefetch_write<Locality=3>` | `void prefetch_write(void*) noexcept` | `__builtin_prefetch(p,1,L)` |
| `prefetch_ahead<T, Ahead=4>` | `void prefetch_ahead(const T* arr, size_t cur, size_t count) noexcept` | prefetch `arr[cur+Ahead]` if in range |
| `QBUEM_LIKELY(x)` / `QBUEM_UNLIKELY(x)` | macros | `__builtin_expect` branch hints |
| `QBUEM_HOT` / `QBUEM_COLD` | macros | `__attribute__((hot/cold))` |
| `CacheLinePad` | `struct { char pad[kCacheLineSize]; }` | manual padding |
| `compiler_barrier()` | `void compiler_barrier() noexcept` | `asm volatile("" ::: "memory")` — compiler reordering only |
| `cpu_pause()` | `void cpu_pause() noexcept` | x86 `PAUSE`, ARM (incl. aarch64) `YIELD`, else no-op |

```cpp
#include <qbuem/core/cpu_hints.hpp>
using namespace qbuem;

// Separate hot atomics onto their own cache lines:
struct Counter {
  alignas(kCacheLineSize) std::atomic<uint64_t> producer{0};
  alignas(kCacheLineSize) std::atomic<uint64_t> consumer{0};
};

// Bounded spin-wait with the right pause instruction (Pillar L2):
while (!ready.load(std::memory_order_acquire))
  cpu_pause();

// Prefetch ahead in a sequential scan:
for (size_t i = 0; i < n; ++i) {
  prefetch_ahead(arr, i, n);
  process(arr[i]);
}
```

**Gotcha:** `compiler_barrier()` only stops compiler reordering — for a true CPU
barrier use `std::atomic_thread_fence(std::memory_order_seq_cst)`. On non-x86/ARM
targets `cpu_pause()` falls back to a compiler barrier (still correct, just no HW
pause).

---

## `MicroTicker` — sub-millisecond heartbeat

`<qbuem/core/micro_ticker.hpp>`

### What it is / role
An **active** drive loop achieving deterministic sub-100 µs ticks via
`nanosleep` for the coarse wait + a busy-spin for the final ~10 µs + per-tick
drift compensation. It does not replace a reactor — it **drives** one via
`poll(0)`. Compared to passive `poll(1ms)` (±500 µs jitter), `MicroTicker` reaches
<5 µs jitter.

| Member | Signature |
|---|---|
| types | `using Clock = std::chrono::steady_clock; using Duration = std::chrono::nanoseconds;` |
| ctor | `explicit MicroTicker(Duration interval, Duration spin_threshold = std::chrono::microseconds{10}) noexcept` |
| `run` | `template <std::invocable<uint64_t> F> void run(F&& callback)` — blocks; `callback(tick_index)` per tick |
| `stop` | `void stop() noexcept` — thread-safe, may be called from any thread/signal handler |
| `running` | `bool running() const noexcept` |
| `interval` | `Duration interval() const noexcept` |

### When to use it
HFT matching loops, real-time physics, sensor-fusion (`examples/11-advanced-apps/`),
or any loop needing a stable mean frequency below the OS scheduler tick.
**When not to:** ordinary servers — a `Dispatcher` with `poll(1ms)` is cheaper and
does not burn a core spinning.

```cpp
#include <qbuem/core/micro_ticker.hpp>
#if defined(__linux__)
#  include <qbuem/core/epoll_reactor.hpp>
   using PlatformReactor = qbuem::EpollReactor;
#else
#  include <qbuem/core/kqueue_reactor.hpp>
   using PlatformReactor = qbuem::KqueueReactor;
#endif
using namespace qbuem;
using namespace std::chrono_literals;

auto reactor = std::make_unique<PlatformReactor>();
MicroTicker ticker(100us);                 // 100 µs target, 10 kHz

ticker.run([&](uint64_t tick) {
  reactor->poll(0);                        // non-blocking: fire ready callbacks
  // ... per-tick application logic ...
  if (tick + 1 >= 200) ticker.stop();
});
```

### Gotchas / constraints
- `run()` **blocks and busy-spins** — give it a dedicated, CPU-pinned thread
  (use NUMA's `pin_reactor_to_cpu`, or pin the OS thread before calling `run()`).
  On Linux, `isolcpus` further reduces jitter.
- The callback **must finish before the next tick deadline**; overrun snaps the
  deadline forward to avoid a death spiral, but you lose the missed tick.
- The doc comment inside the header shows illustrative `create_reactor()` /
  `pin_thread_to_cpu()` calls — **those are not real symbols.** Instantiate
  reactors directly (as above) and pin via `pin_reactor_to_cpu` (NUMA) or your own
  pthread affinity call. See the runnable `examples/01-foundation/micro_ticker/`.

---

## NUMA & CPU affinity — `numa.hpp`

`<qbuem/core/numa.hpp>` (includes `<qbuem/core/dispatcher.hpp>`)

### What it is / role
Pins `Dispatcher` workers to CPUs/NUMA nodes and exposes a PMU counter wrapper.
**All functions are no-ops / safe fallbacks off Linux** (Mac aarch64 returns
`false`/`0`/empty), so the same code compiles and runs everywhere.

| Symbol | Signature | Off-Linux behaviour |
|---|---|---|
| `pin_reactor_to_cpu` | `bool pin_reactor_to_cpu(Dispatcher&, size_t reactor_idx, int cpu_id) noexcept` | returns `false` |
| `numa_node_count` | `int numa_node_count() noexcept` | returns `1` |
| `cpu_to_numa_map` | `std::vector<int> cpu_to_numa_map()` | all-zeros vector |
| `auto_numa_bind` | `size_t auto_numa_bind(Dispatcher&)` | returns `0` |
| `PerfCounters` | class — `start()`, `Snapshot stop()`, `bool available()` | `available()==false`, snapshot all-zero |
| `PerfCounters::Snapshot` | `{ uint64_t cycles, instructions, llc_misses, branch_misses; double ipc() const; }` | — |

```cpp
#include <qbuem/core/numa.hpp>
using namespace qbuem;

Dispatcher disp(2);
std::jthread t([&]{ disp.run(); });        // run() blocks → run it on a thread

pin_reactor_to_cpu(disp, 0, 0);            // worker 0 → CPU 0 (Linux only)
size_t bound = auto_numa_bind(disp);       // spread workers across NUMA nodes

PerfCounters perf;
if (perf.available()) {                     // requires CAP_PERFMON on Linux
  perf.start();
  // ... workload ...
  auto s = perf.stop();
  // s.ipc(), s.cycles, s.llc_misses, ...
}

disp.stop(); t.join();
```

**Gotcha:** `pin_reactor_to_cpu` works by `post_to(idx, ...)` so the worker sets
its own affinity from within its own thread — it therefore only takes effect once
`run()` is active (see `examples/03-memory/numa_hugepages/`). `PerfCounters`
needs `CAP_PERFMON`/appropriate `perf_event_paranoid`, else it degrades to zeros.

---

## Huge pages — `huge_pages.hpp`

`<qbuem/core/huge_pages.hpp>`

### What it is / role
`HugeBufferPool<N, Count>` allocates `Count` fixed-size (`N`-byte) buffers in a
single `mmap(MAP_HUGETLB)` region to minimize TLB misses, with **graceful
fallback**: `MAP_HUGETLB` → ordinary `MAP_ANONYMOUS` (on `ENOMEM`/`EPERM`) →
`new std::byte[]` on non-Linux. Buffers are loaned via `acquire()` /
`release()` and managed by a free-list under an internal `std::mutex`
(thread-safe, unlike Arena/pool).

| Member | Signature |
|---|---|
| ctor | `HugeBufferPool()` — maps `N * Count` bytes upfront |
| `acquire` | `[[nodiscard]] std::span<std::byte> acquire() noexcept` — empty span if exhausted |
| `release` | `void release(std::span<std::byte> buf) noexcept` |
| `available` | `std::size_t available() const noexcept` |
| `capacity` (static) | `static constexpr std::size_t capacity() noexcept` → `Count` |

`static_assert`s require `N > 0` and `Count > 0`. Not copyable/movable.

```cpp
#include <qbuem/core/huge_pages.hpp>
using namespace qbuem;

HugeBufferPool<2 * 1024 * 1024, 4> pool;     // 4 × 2 MiB (tries huge pages)

auto buf = pool.acquire();                    // std::span<std::byte>, size N
if (!buf.empty()) {
  std::memset(buf.data(), 0, 64);
  pool.release(buf);                          // return to pool
}
```

### When to use / gotchas
Use for large DMA-friendly buffers in throughput-critical paths on Linux. On Mac
aarch64 you still get a working `new[]`-backed pool (no actual huge pages — that's
the graceful fallback, not a hard requirement). Enabling real huge pages on Linux
needs `vm.nr_hugepages` configured / `CAP_IPC_LOCK`; without them the pool quietly
uses normal pages. Constructor throws `std::bad_alloc` only if **all** strategies
fail. See `examples/03-memory/numa_hugepages/`.

---

## Service-layer interfaces: `ISessionStore` & `ITransport`

These are **abstract interfaces** the core defines but does not implement —
injection points for service-layer code (Redis/in-memory session stores,
OpenSSL/mbedTLS TLS). Implementing them is how you keep the core zero-dependency.

### `ISessionStore` — `<qbuem/core/session_store.hpp>`
Session-id → serialized-string mapping with TTL. **Implementations may be called
concurrently and must be thread-safe.** All methods are coroutines returning
`Task<Result<...>>`.

| Method | Signature |
|---|---|
| `get` | `Task<Result<std::optional<std::string>>> get(std::string_view session_id)` — `nullopt` if missing/expired |
| `set` | `Task<Result<void>> set(std::string_view, std::string value, std::chrono::seconds ttl = 3600s)` |
| `del` | `Task<Result<void>> del(std::string_view)` — deleting a missing session is not an error |
| `touch` | `Task<Result<void>> touch(std::string_view, std::chrono::seconds ttl = 3600s)` — sliding expiry |
| `exists` | `Task<Result<bool>> exists(std::string_view)` — default impl delegates to `get() != nullopt` |

```cpp
#include <qbuem/core/session_store.hpp>
using namespace qbuem;

Task<Result<void>> handle(ISessionStore& store, std::string_view sid) {
  auto r = co_await store.get(sid);
  if (!r) co_return unexpected(r.error());   // error path
  if (!r->has_value()) {
    // no session — create one
    co_await store.set(sid, R"({"user":"alice"})", std::chrono::seconds{1800});
  }
  co_return Result<void>{};
}
```

### `ITransport` — `<qbuem/core/transport.hpp>`
A bidirectional stream abstraction so plain TCP and a TLS wrapper look identical
to upper layers. qbuem-stack **does not implement TLS** — inject an OpenSSL/
mbedTLS/BoringSSL impl. All I/O methods are non-blocking coroutines.

| Method | Signature | Notes |
|---|---|---|
| `read` | `Task<Result<size_t>> read(std::span<std::byte> buf)` | `0` = EOS; zero-copy span in |
| `write` | `Task<Result<size_t>> write(std::span<const std::byte> buf)` | sends all of `buf` |
| `handshake` | `Task<Result<void>> handshake()` | plain TCP: `ok()` no-op; TLS: must finish before read/write |
| `close` | `Task<Result<void>> close()` | idempotent |
| `negotiated_protocol` | `std::string_view negotiated_protocol() const noexcept` | ALPN: `"h2"`, `"http/1.1"`, or `""` |
| `peer_certificate_fingerprint` | `std::string_view peer_certificate_fingerprint() const noexcept` | mTLS; `""` if unsupported |

```cpp
#include <qbuem/core/transport.hpp>
using namespace qbuem;

Task<Result<void>> serve(ITransport& t) {
  if (auto h = co_await t.handshake(); !h) co_return unexpected(h.error());

  std::byte buf[4096];
  for (;;) {
    auto n = co_await t.read(buf);
    if (!n) co_return unexpected(n.error());   // I/O error
    if (*n == 0) break;                         // peer closed (EOS)
    if (auto w = co_await t.write(std::span<const std::byte>{buf, *n}); !w)
      co_return unexpected(w.error());
  }
  co_await t.close();
  co_return Result<void>{};
}
```

**Constraints (both interfaces):** zero-copy spans (`ITransport`) and string views
must outlive the call; `read`/`write` must not be called after `close()`; the
implementing object is service-layer concern — the core only sees the interface.

---

## Cross-cutting rules for this module group

- **Errors are values.** Check `if (!r)` and read `r.error()` (a
  `std::error_code`); construct failures with
  `return unexpected(std::make_error_code(std::errc::...))`. Never `throw`.
- **Thread confinement.** `Task`, `Arena`, `FixedPoolResource`, `TimerWheel`, and
  the reactor's non-`post`/`stop` methods are all single-thread. One per reactor
  thread; cross threads only via `Reactor::post` / `Dispatcher::post`.
  `HugeBufferPool`, `AsyncLogger`, `ISessionStore` are the thread-safe exceptions.
- **Lifetimes.** Arena/pool/huge-page pointers and spans die with `reset()` /
  `release()` / the owner. io_uring fixed buffers must outlive their registration.
  `AsyncLogger::make_callback()` and `MicroTicker`'s reactor must outlive their
  users.
- **Runnable references:** `examples/01-foundation/` (hello_world, async_timer,
  micro_ticker) and `examples/03-memory/` (arena, numa_hugepages,
  zero_copy_arena_channel, lockfree_bench) exercise every type above with real,
  compiling code.
