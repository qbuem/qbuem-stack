#include <qbuem/core/io_uring_reactor.hpp>

#include <liburing.h>
#include <poll.h>

#include <cerrno>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <sys/eventfd.h>
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
  enum class OpKind { ReadPoll, WritePoll, Timer, Wake };
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
    int res;
  };
  std::vector<RawEvent> events;

  // `cqe` already points to the first completion; consume it then peek more.
  do {
    events.push_back({io_uring_cqe_get_data64(cqe), cqe->res});
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
      read(impl_->wake_fd, &val, sizeof(val));
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
  write(impl_->wake_fd, &one, sizeof(one)); // best-effort
}

void IOUringReactor::stop() { impl_->running = false; }
bool IOUringReactor::is_running() const { return impl_->running; }
bool IOUringReactor::is_sqpoll() const noexcept { return impl_->sqpoll_enabled; }

} // namespace qbuem
