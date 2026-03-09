#include <draco/core/reactor.hpp>

namespace draco {

static thread_local Reactor *thread_reactor = nullptr;

Reactor *Reactor::current() { return thread_reactor; }

void Reactor::set_current(Reactor *r) { thread_reactor = r; }

} // namespace draco
