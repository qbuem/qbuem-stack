/**
 * @file examples/02-network/ws_game_server/ws_game_server.cpp
 * @brief High-level WsServer demo — a real-time room/broadcast game lobby.
 *
 * Shows the production WebSocket layer (`qbuem::WsServer`, not the low-level
 * single-connection `WebSocketHandler`):
 *   - lifecycle callbacks: on_open / on_message / on_close
 *   - per-connection application context (a `Player`)
 *   - a named room ("lobby") with broadcast to all members
 *   - a server-driven fixed-rate "tick" that broadcasts to the room
 *   - non-blocking I/O on a single reactor thread
 *
 * Build:  cmake --build build --target ws_game_server
 * Run:    ./build/examples/ws_game_server          # listens on ws://localhost:9001
 *
 * Try it from a browser console:
 *   const ws = new WebSocket("ws://localhost:9001");
 *   ws.onmessage = e => console.log(e.data);
 *   ws.onopen    = () => ws.send("hello");
 */

#include <qbuem/compat/print.hpp>
#include <qbuem/core/awaiters.hpp>
#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/server/ws_server.hpp>

#include <string>

using namespace qbuem;
using std::println;

namespace {

// Per-connection application state. Attached to every WsConnection at zero
// extra allocation cost (stored inline in the connection object).
struct Player {
  uint64_t    id = 0;
  std::string name;
};

constexpr std::string_view kRoom = "lobby";

// Fixed-rate broadcast loop ("game tick"): every 2 s, push the current player
// count to everyone in the room. Demonstrates server-initiated broadcast from
// the reactor thread (no locks — same thread as the I/O callbacks).
Task<void> tick_loop(WsServer<Player>* server) {
  for (;;) {
    co_await sleep(2000);
    std::string msg =
        "[tick] players online: " + std::to_string(server->room_size(kRoom));
    server->broadcast_room_text(kRoom, msg);
  }
}

} // namespace

int main() {
  Dispatcher disp(1); // single reactor thread owns the server + all connections

  WsServer<Player>* server_ptr = nullptr;

  WsServer<Player> server(WsHandlers<Player>{
      .on_open =
          [&](std::shared_ptr<WsConnection<Player>> conn) {
            conn->context().id   = conn->id();
            conn->context().name = "player" + std::to_string(conn->id());
            server_ptr->join_room(conn->id(), kRoom);

            println("[ws] {} connected ({} online)", conn->context().name,
                    server_ptr->connection_count());
            conn->send_text("welcome, " + conn->context().name);
            server_ptr->broadcast_room_text(
                kRoom, conn->context().name + " joined");
          },
      .on_message =
          [&](std::shared_ptr<WsConnection<Player>> conn, WsMessage msg) {
            if (!msg.is_text) {
              // Binary frames would carry game input in a real game; echo here.
              conn->send_binary(msg.bytes());
              return;
            }
            // Relay chat to the whole room.
            server_ptr->broadcast_room_text(
                kRoom, conn->context().name + ": " + std::string(msg.text()));
          },
      .on_close =
          [&](std::shared_ptr<WsConnection<Player>> conn, uint16_t code) {
            println("[ws] {} disconnected (code {})", conn->context().name,
                    code);
            server_ptr->broadcast_room_text(kRoom,
                                            conn->context().name + " left");
          },
  });
  server_ptr = &server;

  disp.spawn(tick_loop(&server));
  disp.spawn([](WsServer<Player>* s) -> Task<void> {
    auto r = co_await s->listen(9001);
    if (!r) println("[ws] listen failed: {}", r.error().message());
    co_return;
  }(&server));

  println("[ws] game lobby listening on ws://localhost:9001 (Ctrl-C to stop)");
  disp.run(); // blocks, serving connections
  return 0;
}
