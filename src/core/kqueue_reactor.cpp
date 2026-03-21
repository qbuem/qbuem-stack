#include <qbuem/core/kqueue_reactor.hpp>
#include <unistd.h>
#include <chrono>

namespace qbuem {

KqueueReactor::KqueueReactor() 
    : entry_pool_(4096),
      timer_wheel_(4096) {
    kq_fd_ = kqueue();
    if (kq_fd_ == -1) {
        throw std::runtime_error("Failed to create kqueue");
    }

    // Pre-allocate for common use cases
    changelist_.reserve(128);
    events_.resize(128);
    entry_map_.resize(256, nullptr);

    // Register WAKE_IDENT for post()
    struct kevent ev;
    EV_SET(&ev, WAKE_IDENT, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    kevent(kq_fd_, &ev, 1, nullptr, 0, nullptr);

    auto now = std::chrono::steady_clock::now();
    last_tick_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()).count();
}

KqueueReactor::~KqueueReactor() {
    if (kq_fd_ != -1) {
        close(kq_fd_);
    }
    // FixedPoolResource handles memory allocation, but we must call destructors
    // because KqueueEntry contains std::function (non-trivial).
    for (auto* entry : entry_map_) {
        if (entry) {
            entry->~KqueueEntry();
        }
    }
}

Result<void> KqueueReactor::register_event(int fd, EventType type,
                                         std::function<void(int)> callback) {
    auto* entry = get_or_create_entry(fd);
    if (!entry) return std::unexpected(std::make_error_code(std::errc::not_enough_memory));

    short filter = (type == EventType::Read) ? EVFILT_READ : EVFILT_WRITE;
    if (type == EventType::Read) entry->read_cb = std::move(callback);
    else entry->write_cb = std::move(callback);

    struct kevent ev;
    // Use EV_CLEAR for Edge-Triggered behavior per optimization guide.
    EV_SET(&ev, fd, filter, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, entry);
    changelist_.push_back(ev);
    entry->active = true;

    return Result<void>{};
}

Result<void> KqueueReactor::unregister_event(int fd, EventType type) {
    if (static_cast<size_t>(fd) >= entry_map_.size() || !entry_map_[fd]) {
        return Result<void>{};
    }

    auto* entry = entry_map_[fd];
    short filter = (type == EventType::Read) ? EVFILT_READ : EVFILT_WRITE;

    struct kevent ev;
    EV_SET(&ev, fd, filter, EV_DELETE, 0, 0, nullptr);
    changelist_.push_back(ev);

    if (type == EventType::Read) entry->read_cb = nullptr;
    else entry->write_cb = nullptr;

    if (!entry->read_cb && !entry->write_cb) {
        entry->active = false;
    }

    return Result<void>{};
}

Result<int> KqueueReactor::register_timer(int timeout_ms,
                                        std::function<void(int)> callback) {
    // We need to pass the ID back to the callback, but the ID isn't known 
    // until schedule() returns. We use a shared pointer to bridge this.
    struct TimerCtx {
        std::function<void(int)> cb;
        TimerWheel::TimerId id;
    };
    auto ctx = std::make_shared<TimerCtx>();
    ctx->cb = std::move(callback);
    
    ctx->id = timer_wheel_.schedule(timeout_ms, [ctx]() {
        if (ctx->cb) ctx->cb(static_cast<int>(ctx->id));
    });

    if (ctx->id == TimerWheel::kInvalid) {
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }
    
    return static_cast<int>(ctx->id);
}

Result<void> KqueueReactor::register_signal(int sig, std::function<void(int)> callback) {
    auto* entry = get_or_create_entry(sig);
    if (!entry) return std::unexpected(std::make_error_code(std::errc::not_enough_memory));

    entry->signal_cb = std::move(callback);

    struct kevent ev;
    EV_SET(&ev, sig, EVFILT_SIGNAL, EV_ADD | EV_ENABLE, 0, 0, entry);
    changelist_.push_back(ev);
    entry->active = true;

    return Result<void>{};
}

Result<void> KqueueReactor::unregister_signal(int sig) {
    if (static_cast<size_t>(sig) >= entry_map_.size() || !entry_map_[sig]) {
        return Result<void>{};
    }

    auto* entry = entry_map_[sig];
    struct kevent ev;
    EV_SET(&ev, sig, EVFILT_SIGNAL, EV_DELETE, 0, 0, nullptr);
    changelist_.push_back(ev);

    entry->signal_cb = nullptr;
    if (!entry->read_cb && !entry->write_cb && !entry->signal_cb) {
        entry->active = false;
    }

    return Result<void>{};
}

Result<void> KqueueReactor::unregister_timer(int timer_id) {
    timer_wheel_.cancel(static_cast<TimerWheel::TimerId>(timer_id));
    return Result<void>{};
}

Result<int> KqueueReactor::poll(int timeout_ms) {
    flush_changes();

    // 1. Calculate timeout considering TimerWheel
    uint64_t wheel_timeout = timer_wheel_.next_expiry_ms();
    int final_timeout_ms = timeout_ms;
    if (wheel_timeout != std::numeric_limits<uint64_t>::max()) {
        final_timeout_ms = std::min(static_cast<int>(wheel_timeout), timeout_ms);
    }

    struct timespec ts;
    struct timespec* ts_ptr = nullptr;
    if (final_timeout_ms >= 0) {
        ts.tv_sec = final_timeout_ms / 1000;
        ts.tv_nsec = (final_timeout_ms % 1000) * 1000000;
        ts_ptr = &ts;
    }

    int n = kevent(kq_fd_, nullptr, 0, events_.data(), events_.size(), ts_ptr);
    if (n < 0) {
        if (errno == EINTR) return Result<int>{0};
        return std::unexpected(std::error_code(errno, std::system_category()));
    }

    // 2. Dispatch events
    for (int i = 0; i < n; ++i) {
        auto& ev = events_[i];
        if (ev.filter == EVFILT_USER) {
            std::vector<std::function<void()>> local_queue;
            {
                std::lock_guard<std::mutex> lock(work_mutex_);
                local_queue.swap(work_queue_);
            }
            for (auto& fn : local_queue) fn();
            continue;
        }

        auto* entry = static_cast<KqueueEntry*>(ev.udata);
        if (!entry || !entry->active) continue;

        if (ev.flags & EV_ERROR) {
            // Handle error (EV_ERROR is stored in data)
            continue;
        }

        if (ev.filter == EVFILT_SIGNAL) {
            if (entry->signal_cb) {
                entry->signal_cb(static_cast<int>(ev.ident));
            }
        } else if (ev.filter == EVFILT_READ) {
            if (entry->read_cb) entry->read_cb(entry->ident);
            // Handle EOF (peer closed) - we might want to trigger read_cb 
            // anyway to let it see 0 bytes read, but EV_EOF is a nice hint.
            if (ev.flags & EV_EOF && entry->read_cb) {
                // entry->read_cb(entry->ident); // already called or could be specialized
            }
        } else if (ev.filter == EVFILT_WRITE && entry->write_cb) {
            entry->write_cb(entry->ident);
        }
    }

    // 3. Update TimerWheel
    auto now = std::chrono::steady_clock::now();
    uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()).count();
    uint64_t elapsed = (now_ms > last_tick_ms_) ? (now_ms - last_tick_ms_) : 0;
    
    if (elapsed > 0) {
        timer_wheel_.tick(elapsed);
        last_tick_ms_ = now_ms;
    }

    return Result<int>{n};
}

void KqueueReactor::stop() {
    running_ = false;
    post([]() {}); // Wake up
}

bool KqueueReactor::is_running() const {
    return running_;
}

void KqueueReactor::post(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lock(work_mutex_);
        work_queue_.push_back(std::move(fn));
    }
    struct kevent ev;
    EV_SET(&ev, WAKE_IDENT, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    kevent(kq_fd_, &ev, 1, nullptr, 0, nullptr);
}

void KqueueReactor::flush_changes() {
    if (changelist_.empty()) return;
    kevent(kq_fd_, changelist_.data(), changelist_.size(), nullptr, 0, nullptr);
    changelist_.clear();
}

KqueueReactor::KqueueEntry* KqueueReactor::get_or_create_entry(int fd) {
    if (fd < 0) return nullptr;
    if (static_cast<size_t>(fd) >= entry_map_.size()) {
        entry_map_.resize(fd + 1, nullptr);
    }
    if (entry_map_[fd]) return entry_map_[fd];

    void* raw = entry_pool_.allocate();
    if (!raw) return nullptr;
    auto* entry = new (raw) KqueueEntry();
    entry->ident = fd;
    entry_map_[fd] = entry;
    return entry;
}

} // namespace qbuem
