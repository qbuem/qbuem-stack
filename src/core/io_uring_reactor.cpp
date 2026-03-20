#include <qbuem/core/io_uring_reactor.hpp>

#include <liburing.h>
#include <poll.h>

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace qbuem {

// ---------------------------------------------------------------------------
// Internal implementation details hidden behind the Pimpl.
// ---------------------------------------------------------------------------
struct IOUringReactor::Impl {
  struct io_uring ring;
  bool running = true;
  bool sqpoll_enabled = false; // true when IORING_SETUP_SQPOLL was accepted
  uint64_t next_token = 1; // Per-op unique ID used as SQE user_data
  int next_timer_id = 1;

  // Every pending io_uring operation is described by an Op.
  enum class OpKind { ReadPoll, WritePoll, Timer, Wake, ReadFixed, WriteFixed, BufRingRecv };
  struct Op {
    OpKind kind;
    int fd = -1;        // valid for ReadPoll / WritePoll
    int timer_id = -1;  // valid for Timer
    std::function<void(int)> callback;
    struct __kernel_timespec ts{}; // lifetime for Timer SQEs
  };

  // token → Op (owns the callback + timer timespec until CQE arrives)
  std::unordered_map<uint64_t, Op> ops;

  // Track the active poll token for each fd so we can cancel or resubmit.
  std::unordered_map<int, uint64_t> read_tokens;  // fd → token
  std::unordered_map<int, uint64_t> write_tokens; // fd → token
  std::unordered_map<int, uint64_t> timer_tokens; // timer_id → token

  // Fixed Buffer state (io_uring_register_buffers)
  size_t fixed_buf_count = 0; // number of currently registered fixed buffers

  // Buffer Ring state (IORING_OP_PROVIDE_BUFFERS / io_uring_register_buf_ring)
  struct BufRingEntry {
    io_uring_buf_ring *br    = nullptr;
    size_t             buf_size  = 0;
    size_t             buf_count = 0;
    std::vector<uint8_t> memory; // backing store for all buffers in the ring
    std::function<void(int, uint16_t, void *)> pending_cb; // active recv cb
    int  pending_fd = -1;
    uint16_t bgid   = 0;
  };
  std::unordered_map<uint16_t, BufRingEntry> buf_rings; // bgid → entry

  // post() wakeup: eventfd + work queue
  int wake_fd = -1;
  uint64_t wake_token = 0;
  std::mutex wake_mutex;
  std::vector<std::function<void()>> wake_queue;

  // Submit a fresh POLL_ADD for wake_fd and record it.
  void resubmit_wake(uint64_t token) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
      io_uring_submit(&ring);
      sqe = io_uring_get_sqe(&ring);
      if (!sqe)
        return;
    }
    io_uring_prep_poll_add(sqe, wake_fd, POLLIN);
    io_uring_sqe_set_data64(sqe, token);
  }

  // Submit a POLL_ADD SQE for a pending op that is already in `ops`.
  void submit_poll(uint64_t token, int fd, EventType type) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
      io_uring_submit(&ring);
      sqe = io_uring_get_sqe(&ring);
      if (!sqe)
        return; // Ring overflow – event lost; will be rescheduled next poll.
    }
    unsigned mask = (type == EventType::Read) ? POLLIN : POLLOUT;
    io_uring_prep_poll_add(sqe, fd, mask);
    io_uring_sqe_set_data64(sqe, token);
  }

  // Submit an ASYNC_CANCEL SQE to cancel a pending operation by token.
  void cancel_by_token(uint64_t token) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
      io_uring_submit(&ring);
      sqe = io_uring_get_sqe(&ring);
      if (!sqe)
        return;
    }
    // user_data=0 for the cancel SQE → no callback dispatched on completion.
    io_uring_prep_cancel64(sqe, token, 0);
    io_uring_sqe_set_data64(sqe, 0);
  }
};

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
IOUringReactor::IOUringReactor() : impl_(new Impl{}) {
  // Attempt SQPOLL mode first (steady-state syscall-free polling).
  // IORING_SETUP_SQPOLL requires either CAP_SYS_ADMIN or a recent kernel
  // with unprivileged SQPOLL support.  We fall back gracefully if unavailable.
  // Note: SINGLE_ISSUER is intentionally omitted — the Dispatcher may call
  // register_event from the main thread before the reactor thread starts.
  int ret = io_uring_queue_init(IOUringReactor::QUEUE_DEPTH, &impl_->ring,
                                IORING_SETUP_SQPOLL);
  if (ret == -EPERM || ret == -EINVAL) {
    // Kernel doesn't support SQPOLL or insufficient privileges — use plain mode.
    ret = io_uring_queue_init(IOUringReactor::QUEUE_DEPTH, &impl_->ring, 0);
  } else if (ret == 0) {
    impl_->sqpoll_enabled = true;
  }
  if (ret < 0)
    throw std::runtime_error(std::string("io_uring_queue_init failed: ") +
                             strerror(-ret));

  // Create eventfd for post() wakeup and register a persistent POLL_ADD.
  impl_->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (impl_->wake_fd == -1) {
    io_uring_queue_exit(&impl_->ring);
    throw std::runtime_error("eventfd failed");
  }

  uint64_t token = impl_->next_token++;
  impl_->wake_token = token;
  auto &wake_op = impl_->ops[token];
  wake_op.kind = Impl::OpKind::Wake;
  wake_op.fd = impl_->wake_fd;
  impl_->resubmit_wake(token);
  io_uring_submit(&impl_->ring);
}

IOUringReactor::~IOUringReactor() {
  if (impl_->wake_fd != -1)
    close(impl_->wake_fd);
  io_uring_queue_exit(&impl_->ring);
  delete impl_;
}

// ---------------------------------------------------------------------------
// register_event  →  POLL_ADD  (oneshot; reactor resubmits for persistence)
// ---------------------------------------------------------------------------
Result<void> IOUringReactor::register_event(int fd, EventType type,
                                            std::function<void(int)> callback) {
  uint64_t token = impl_->next_token++;
  auto &op = impl_->ops[token];
  op.kind = (type == EventType::Read) ? Impl::OpKind::ReadPoll
                                      : Impl::OpKind::WritePoll;
  op.fd = fd;
  op.callback = std::move(callback);

  if (type == EventType::Read)
    impl_->read_tokens[fd] = token;
  else
    impl_->write_tokens[fd] = token;

  impl_->submit_poll(token, fd, type);
  io_uring_submit(&impl_->ring);
  return {};
}

// ---------------------------------------------------------------------------
// unregister_event  →  ASYNC_CANCEL on the pending POLL_ADD
// ---------------------------------------------------------------------------
Result<void> IOUringReactor::unregister_event(int fd, EventType type) {
  auto &tmap =
      (type == EventType::Read) ? impl_->read_tokens : impl_->write_tokens;
  auto it = tmap.find(fd);
  if (it == tmap.end())
    return {};

  uint64_t token = it->second;
  tmap.erase(it);
  impl_->ops.erase(token); // Remove callback ownership; ignore pending CQE.
  impl_->cancel_by_token(token);
  io_uring_submit(&impl_->ring);
  return {};
}

// ---------------------------------------------------------------------------
// register_timer  →  IORING_OP_TIMEOUT
// ---------------------------------------------------------------------------
Result<int> IOUringReactor::register_timer(int timeout_ms,
                                           std::function<void(int)> callback) {
  int timer_id = impl_->next_timer_id++;
  uint64_t token = impl_->next_token++;

  auto &op = impl_->ops[token];
  op.kind = Impl::OpKind::Timer;
  op.timer_id = timer_id;
  op.callback = std::move(callback);
  // The kernel reads ts during SQE processing (synchronous), so it is safe
  // to store it inside the Op and free it when the Op is erased.
  op.ts.tv_sec = timeout_ms / 1000;
  op.ts.tv_nsec = (timeout_ms % 1000) * 1'000'000LL;

  impl_->timer_tokens[timer_id] = token;

  struct io_uring_sqe *sqe = io_uring_get_sqe(&impl_->ring);
  if (!sqe) {
    io_uring_submit(&impl_->ring);
    sqe = io_uring_get_sqe(&impl_->ring);
    if (!sqe) {
      impl_->ops.erase(token);
      impl_->timer_tokens.erase(timer_id);
      return unexpected(
          std::make_error_code(std::errc::resource_unavailable_try_again));
    }
  }
  io_uring_prep_timeout(sqe, &op.ts, 0, 0);
  io_uring_sqe_set_data64(sqe, token);
  io_uring_submit(&impl_->ring);
  return timer_id;
}

// ---------------------------------------------------------------------------
// unregister_timer  →  ASYNC_CANCEL on the pending TIMEOUT op
// ---------------------------------------------------------------------------
Result<void> IOUringReactor::unregister_timer(int timer_id) {
  auto it = impl_->timer_tokens.find(timer_id);
  if (it == impl_->timer_tokens.end())
    return {}; // Already fired or never registered.

  uint64_t token = it->second;
  impl_->timer_tokens.erase(it);
  impl_->ops.erase(token);
  impl_->cancel_by_token(token);
  io_uring_submit(&impl_->ring);
  return {};
}

// ---------------------------------------------------------------------------
// poll  →  io_uring_wait_cqe_timeout + batch drain
// ---------------------------------------------------------------------------
Result<int> IOUringReactor::poll(int timeout_ms) {
  struct __kernel_timespec ts{};
  struct __kernel_timespec *ts_ptr = nullptr;
  if (timeout_ms >= 0) {
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = (timeout_ms % 1000) * 1'000'000LL;
    ts_ptr = &ts;
  }

  // Wait for at least one CQE.  io_uring_wait_cqe_timeout submits any
  // pending SQEs internally and blocks up to `ts` for a completion.
  struct io_uring_cqe *cqe;
  int ret = io_uring_wait_cqe_timeout(&impl_->ring, &cqe, ts_ptr);
  if (ret == -ETIME || ret == -EINTR)
    return 0;
  if (ret < 0)
    return unexpected(std::make_error_code(std::errc::io_error));

  // Collect and consume CQEs one at a time so every advance is accounted for.
  struct RawEvent {
    uint64_t token;
    int      res;
    unsigned flags; // CQE flags (e.g. IORING_CQE_F_BUFFER for buf-ring recv)
  };
  std::vector<RawEvent> events;

  // `cqe` already points to the first completion; consume it then peek more.
  do {
    events.push_back({io_uring_cqe_get_data64(cqe), cqe->res, cqe->flags});
    io_uring_cqe_seen(&impl_->ring, cqe); // advances khead by 1
  } while (io_uring_peek_cqe(&impl_->ring, &cqe) == 0);

  // Process events.  Callbacks may submit new SQEs via register_event etc.
  for (auto &ev : events) {
    if (ev.token == 0)
      continue; // Cancel-SQE completion – no user callback.

    auto it = impl_->ops.find(ev.token);
    if (it == impl_->ops.end())
      continue; // Op was already cancelled by unregister_event/timer.

    Impl::Op op = std::move(it->second);
    impl_->ops.erase(it);

    if (op.kind == Impl::OpKind::Wake) {
      // Drain the eventfd counter.
      uint64_t val;
      (void)read(impl_->wake_fd, &val, sizeof(val));
      // Drain work queue.
      std::vector<std::function<void()>> local;
      {
        std::lock_guard<std::mutex> lock(impl_->wake_mutex);
        local.swap(impl_->wake_queue);
      }
      // Resubmit POLL_ADD for wake_fd before executing callbacks so any
      // new post() calls queued inside a callback are not lost.
      uint64_t new_token = impl_->next_token++;
      impl_->wake_token = new_token;
      auto &new_op = impl_->ops[new_token];
      new_op.kind = Impl::OpKind::Wake;
      new_op.fd = impl_->wake_fd;
      impl_->resubmit_wake(new_token);
      for (auto &wfn : local)
        wfn();

    } else if (op.kind == Impl::OpKind::ReadPoll ||
               op.kind == Impl::OpKind::WritePoll) {
      EventType etype = (op.kind == Impl::OpKind::ReadPoll) ? EventType::Read
                                                             : EventType::Write;
      auto &tmap = (etype == EventType::Read) ? impl_->read_tokens
                                              : impl_->write_tokens;

      if (ev.res < 0) {
        // Cancelled or error – remove stale token entry.
        tmap.erase(op.fd);
        continue;
      }

      // Persistent-event semantics: resubmit POLL_ADD before invoking the
      // callback so that a new event is already queued while the callback runs.
      if (tmap.count(op.fd)) {
        uint64_t new_token = impl_->next_token++;
        auto &new_op = impl_->ops[new_token];
        new_op.kind = op.kind;
        new_op.fd = op.fd;
        new_op.callback = op.callback; // keep a copy for re-submission
        tmap[op.fd] = new_token;
        impl_->submit_poll(new_token, op.fd, etype);
      }

      if (op.callback)
        op.callback(op.fd);

    } else if (op.kind == Impl::OpKind::ReadFixed ||
               op.kind == Impl::OpKind::WriteFixed) {
      // Fixed-buffer read/write completes once — no resubmit.
      if (op.callback)
        op.callback(ev.res); // positive=bytes, negative=errno

    } else if (op.kind == Impl::OpKind::BufRingRecv) {
      // Buffer-ring recv: kernel selected a buffer from the pool.
      uint16_t bgid = static_cast<uint16_t>(op.timer_id);
      auto ring_it  = impl_->buf_rings.find(bgid);
      if (ring_it == impl_->buf_rings.end())
        continue;

      auto &entry = ring_it->second;
      if (ev.res < 0) {
        if (entry.pending_cb)
          entry.pending_cb(ev.res, 0, nullptr);
      } else {
        // Extract selected buffer ID from CQE flags.
        uint16_t buf_id = ev.flags >> IORING_CQE_BUFFER_SHIFT;
        const size_t ring_hdr_sz =
            sizeof(io_uring_buf_ring) + entry.buf_count * sizeof(io_uring_buf);
        void *buf_ptr =
            entry.memory.data() + ring_hdr_sz + buf_id * entry.buf_size;
        if (entry.pending_cb)
          entry.pending_cb(ev.res, buf_id, buf_ptr);
      }
      entry.pending_cb = {};
      entry.pending_fd = -1;

    } else {
      // Timer
      impl_->timer_tokens.erase(op.timer_id);
      // -ETIME = timer expired (normal); any other negative = cancelled.
      if (ev.res != 0 && ev.res != -ETIME)
        continue;
      if (op.callback)
        op.callback(op.timer_id);
    }
  }

  // Submit SQEs accumulated by callbacks (resubmit_poll, register_event…).
  // In SQPOLL mode the kernel thread polls the SQ ring autonomously, but we
  // still call submit() to wake a sleeping SQPOLL thread (IORING_SQ_NEED_WAKEUP).
  io_uring_submit(&impl_->ring);

  return (int)events.size();
}

void IOUringReactor::post(std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> lock(impl_->wake_mutex);
    impl_->wake_queue.push_back(std::move(fn));
  }
  // Wake the ring by writing to the eventfd; the registered POLL_ADD fires.
  const uint64_t one = 1;
  (void)write(impl_->wake_fd, &one, sizeof(one)); // best-effort wakeup
}

void IOUringReactor::stop() { impl_->running = false; }
bool IOUringReactor::is_running() const { return impl_->running; }
bool IOUringReactor::is_sqpoll() const noexcept { return impl_->sqpoll_enabled; }

// Signal handling via signalfd is not implemented in IOUringReactor.
// Use the kqueue reactor on macOS or wire up signalfd(2) explicitly.
Result<void> IOUringReactor::register_signal(int /*sig*/,
                                             std::function<void(int)> /*cb*/) {
  return std::unexpected(std::make_error_code(std::errc::not_supported));
}

Result<void> IOUringReactor::unregister_signal(int /*sig*/) {
  return std::unexpected(std::make_error_code(std::errc::not_supported));
}

// ---------------------------------------------------------------------------
// Fixed Buffer API
// ---------------------------------------------------------------------------

Result<void>
IOUringReactor::register_fixed_buffers(std::span<const iovec> iovecs) {
  int ret = io_uring_register_buffers(
      &impl_->ring, iovecs.data(), static_cast<unsigned>(iovecs.size()));
  if (ret < 0)
    return unexpected(std::make_error_code(std::errc(-ret)));
  impl_->fixed_buf_count = iovecs.size();
  return {};
}

void IOUringReactor::unregister_fixed_buffers() noexcept {
  io_uring_unregister_buffers(&impl_->ring);
  impl_->fixed_buf_count = 0;
}

size_t IOUringReactor::fixed_buffer_count() const noexcept {
  return impl_->fixed_buf_count;
}

Result<void> IOUringReactor::read_fixed(int fd, int buf_idx,
                                        std::span<std::byte> buf,
                                        int64_t file_offset,
                                        std::function<void(int)> callback) {
  uint64_t token = impl_->next_token++;
  auto &op       = impl_->ops[token];
  op.kind        = Impl::OpKind::ReadFixed;
  op.fd          = fd;
  op.callback    = std::move(callback);

  struct io_uring_sqe *sqe = io_uring_get_sqe(&impl_->ring);
  if (!sqe) {
    io_uring_submit(&impl_->ring);
    sqe = io_uring_get_sqe(&impl_->ring);
    if (!sqe) {
      impl_->ops.erase(token);
      return unexpected(std::make_error_code(std::errc::resource_unavailable_try_again));
    }
  }
  io_uring_prep_read_fixed(sqe, fd, buf.data(), static_cast<unsigned>(buf.size()),
                           file_offset, buf_idx);
  io_uring_sqe_set_data64(sqe, token);
  io_uring_submit(&impl_->ring);
  return {};
}

Result<void> IOUringReactor::write_fixed(int fd, int buf_idx,
                                         std::span<const std::byte> buf,
                                         int64_t file_offset,
                                         std::function<void(int)> callback) {
  uint64_t token = impl_->next_token++;
  auto &op       = impl_->ops[token];
  op.kind        = Impl::OpKind::WriteFixed;
  op.fd          = fd;
  op.callback    = std::move(callback);

  struct io_uring_sqe *sqe = io_uring_get_sqe(&impl_->ring);
  if (!sqe) {
    io_uring_submit(&impl_->ring);
    sqe = io_uring_get_sqe(&impl_->ring);
    if (!sqe) {
      impl_->ops.erase(token);
      return unexpected(std::make_error_code(std::errc::resource_unavailable_try_again));
    }
  }
  // Cast away const: liburing expects void* but won't write to it for write ops.
  io_uring_prep_write_fixed(sqe, fd, const_cast<std::byte *>(buf.data()),
                            static_cast<unsigned>(buf.size()), file_offset,
                            buf_idx);
  io_uring_sqe_set_data64(sqe, token);
  io_uring_submit(&impl_->ring);
  return {};
}

// ---------------------------------------------------------------------------
// Buffer Ring API
// ---------------------------------------------------------------------------

Result<void> IOUringReactor::register_buf_ring(uint16_t bgid, size_t buf_size,
                                               size_t buf_count) {
  if (impl_->buf_rings.contains(bgid))
    return unexpected(std::make_error_code(std::errc::address_in_use));

  auto &entry      = impl_->buf_rings[bgid];
  entry.bgid       = bgid;
  entry.buf_size   = buf_size;
  entry.buf_count  = buf_count;

  // Allocate backing memory for all buffers + the ring header.
  const size_t ring_sz =
      sizeof(io_uring_buf_ring) + buf_count * sizeof(io_uring_buf);
  const size_t data_sz = buf_size * buf_count;
  entry.memory.resize(ring_sz + data_sz, 0);

  auto *br = reinterpret_cast<io_uring_buf_ring *>(entry.memory.data());
  io_uring_buf_ring_init(br);
  entry.br = br;

  // Register with the kernel.
  struct io_uring_buf_reg reg{};
  reg.ring_addr    = reinterpret_cast<uint64_t>(br);
  reg.ring_entries = static_cast<uint32_t>(buf_count);
  reg.bgid         = bgid;

  int ret = io_uring_register_buf_ring(&impl_->ring, &reg, 0);
  if (ret < 0) {
    impl_->buf_rings.erase(bgid);
    return unexpected(std::make_error_code(std::errc(-ret)));
  }

  // Provide all buffers to the kernel.
  uint8_t *data_base = entry.memory.data() + ring_sz;
  const int mask     = static_cast<int>(buf_count) - 1;
  for (size_t i = 0; i < buf_count; ++i) {
    io_uring_buf_ring_add(br, data_base + i * buf_size,
                          static_cast<unsigned>(buf_size),
                          static_cast<uint16_t>(i), mask, static_cast<int>(i));
  }
  io_uring_buf_ring_advance(br, static_cast<int>(buf_count));
  return {};
}

void IOUringReactor::unregister_buf_ring(uint16_t bgid) noexcept {
  auto it = impl_->buf_rings.find(bgid);
  if (it == impl_->buf_rings.end())
    return;
  io_uring_unregister_buf_ring(&impl_->ring, bgid);
  impl_->buf_rings.erase(it);
}

Result<void> IOUringReactor::recv_buffered(
    int fd, uint16_t bgid,
    std::function<void(int, uint16_t, void *)> callback) {

  auto it = impl_->buf_rings.find(bgid);
  if (it == impl_->buf_rings.end())
    return unexpected(std::make_error_code(std::errc::invalid_argument));

  uint64_t token = impl_->next_token++;
  auto &op       = impl_->ops[token];
  op.kind        = Impl::OpKind::BufRingRecv;
  op.fd          = fd;
  // Store metadata in timer_id slot (bgid fits in int).
  op.timer_id    = static_cast<int>(bgid);

  auto &entry    = it->second;
  entry.pending_cb = std::move(callback);
  entry.pending_fd = fd;

  struct io_uring_sqe *sqe = io_uring_get_sqe(&impl_->ring);
  if (!sqe) {
    io_uring_submit(&impl_->ring);
    sqe = io_uring_get_sqe(&impl_->ring);
    if (!sqe) {
      impl_->ops.erase(token);
      return unexpected(std::make_error_code(std::errc::resource_unavailable_try_again));
    }
  }
  io_uring_prep_recv(sqe, fd, nullptr, 0, 0);
  sqe->buf_group = bgid;
  sqe->flags    |= IOSQE_BUFFER_SELECT;
  io_uring_sqe_set_data64(sqe, token);
  io_uring_submit(&impl_->ring);
  return {};
}

void IOUringReactor::return_buf_to_ring(uint16_t bgid,
                                        uint16_t buf_id) noexcept {
  auto it = impl_->buf_rings.find(bgid);
  if (it == impl_->buf_rings.end())
    return;

  auto &entry     = it->second;
  const size_t ring_sz =
      sizeof(io_uring_buf_ring) + entry.buf_count * sizeof(io_uring_buf);
  uint8_t *data_base = entry.memory.data() + ring_sz;
  const int mask     = static_cast<int>(entry.buf_count) - 1;

  io_uring_buf_ring_add(entry.br, data_base + buf_id * entry.buf_size,
                        static_cast<unsigned>(entry.buf_size), buf_id, mask, 0);
  io_uring_buf_ring_advance(entry.br, 1);
}

} // namespace qbuem
