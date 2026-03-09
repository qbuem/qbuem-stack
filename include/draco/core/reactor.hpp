#pragma once

#include <draco/common.hpp>
#include <functional>

namespace draco {

/**
 * @brief Event type for the Reactor.
 */
enum class EventType { Read, Write, Error };

/**
 * @brief Abstract Reactor interface for platform-specific event loops.
 *
 * Each Reactor instance runs on exactly one thread (Shared-Nothing).
 */
class Reactor {
public:
  virtual ~Reactor() = default;

  /**
   * @brief Register a file descriptor with the reactor.
   *
   * @param fd The file descriptor to monitor.
   * @param type The type of event to monitor.
   * @param callback Function to call when the event occurs.
   */
  virtual Result<void> register_event(int fd, EventType type,
                                      std::function<void(int)> callback) = 0;

  /**
   * @brief Register a timer with the reactor.
   *
   * @param timeout_ms Timeout in milliseconds.
   * @param callback Function to call when the timer fires.
   * @return The timer ID.
   */
  virtual Result<int> register_timer(int timeout_ms,
                                     std::function<void(int)> callback) = 0;

  /**
   * @brief Unregister a file descriptor.
   */
  virtual Result<void> unregister_event(int fd, EventType type) = 0;

  /**
   * @brief Unregister a timer.
   */
  virtual Result<void> unregister_timer(int timer_id) = 0;

  /**
   * @brief Check if the reactor is running.
   */
  virtual bool is_running() const = 0;

  /**
   * @brief Run the event loop for a single iteration or until an event occurs.
   *
   * @param timeout_ms Timeout in milliseconds. -1 for infinite.
   */
  virtual Result<int> poll(int timeout_ms) = 0;

  /**
   * @brief Stop the reactor loop.
   */
  virtual void stop() = 0;

  /**
   * @brief Get the Reactor for the current thread.
   */
  static Reactor *current();

  /**
   * @brief Set the Reactor for the current thread.
   */
  static void set_current(Reactor *r);
};

} // namespace draco
