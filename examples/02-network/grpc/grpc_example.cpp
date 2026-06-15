/**
 * @file examples/02-network/grpc/grpc_example.cpp
 * @brief gRPC handler API — Unary / Server-streaming / Client-streaming / Bidi.
 *
 * gRPC sits on the experimental HTTP/2 transport layer. This example drives the
 * transport-independent gRPC core that DOES fully work — the part that turns a
 * request into a response and frames it on the wire — entirely in one process,
 * with no socket. Every line below produces real computed output:
 *
 *   - GrpcHandler<Req,Res>::dispatch_unary()  — deserialize → coroutine handler
 *     → serialize: a complete unary round-trip with no protobuf dependency.
 *   - GrpcHandler<Req,Res>::dispatch_server_stream() / dispatch_client_stream()
 *     / dispatch_bidi() — driven through the real Stream<Res> / AsyncChannel<Req>
 *     interfaces, here backed by tiny in-memory implementations.
 *   - GrpcHandler<Req,Res>::encode_message() / decode_message() — the gRPC
 *     Length-Prefixed Message framing ([flag:1][len:4 BE][payload]) round-tripped
 *     byte-for-byte.
 *   - GrpcHandler<Req,Res>::status_trailer() / grpc_path() — the trailer block
 *     and ":path" pseudo-header a real HTTP/2 gRPC frame would carry.
 *
 * Serialization here is a tiny hand-rolled binary codec (varint + bytes) so the
 * example stays zero-dependency; in production you plug protobuf into the
 * SerializeFn/DeserializeFn exactly the same way.
 *
 * What this example does NOT do: open a TCP socket and run gRPC over a live
 * HTTP/2 connection. That path needs Http2Client / Http2Connection (the
 * experimental h2 layer) plus a running Reactor. See the limitations note in the
 * accompanying report.
 *
 * Coverage:
 *   - GrpcMessage / GrpcStatus
 *   - GrpcHandler<Req,Res>: set_serializer / set_unary / set_server_stream /
 *     set_client_stream / set_bidi
 *   - GrpcHandler<Req,Res>: dispatch_unary / dispatch_server_stream /
 *     dispatch_client_stream / dispatch_bidi
 *   - GrpcHandler<Req,Res>: encode_message / decode_message
 *   - GrpcHandler<Req,Res>: status_trailer / grpc_path / has_unary
 *   - Stream<Res> / AsyncChannel<Req> interfaces (in-memory implementations)
 */

#include <qbuem/server/grpc_handler.hpp>  // GrpcHandler / GrpcMessage / GrpcStatus / Stream / AsyncChannel
#include <qbuem/core/task.hpp>

#include <cassert>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <qbuem/compat/print.hpp>

using namespace qbuem;
using std::println;

// ─── Application message types (protobuf-free) ────────────────────────────────

struct HelloRequest {
    std::string name;
};

struct HelloReply {
    std::string message;
};

// ─── Minimal wire codec (varint length + raw bytes) ───────────────────────────
// Stands in for protobuf SerializeAsBytes / ParseFromArray. Zero dependencies.

static void put_string(std::vector<uint8_t>& out, std::string_view s) {
    uint32_t n = static_cast<uint32_t>(s.size());
    while (n >= 0x80u) { out.push_back(static_cast<uint8_t>(n) | 0x80u); n >>= 7u; }
    out.push_back(static_cast<uint8_t>(n));
    out.insert(out.end(), s.begin(), s.end());
}

static std::string get_string(std::span<const uint8_t> in) {
    uint32_t n = 0, shift = 0;
    size_t i = 0;
    for (; i < in.size(); ++i) {
        n |= static_cast<uint32_t>(in[i] & 0x7Fu) << shift;
        if ((in[i] & 0x80u) == 0u) { ++i; break; }
        shift += 7u;
    }
    return std::string(reinterpret_cast<const char*>(in.data() + i),
                       std::min<size_t>(n, in.size() - i));
}

static GrpcMessage encode_request(std::string_view name) {
    std::vector<uint8_t> payload;
    put_string(payload, name);
    return GrpcMessage{ /*compressed=*/false, std::move(payload) };
}

// ─── In-memory Stream<Res> — captures server → client messages ────────────────
// Implements the gRPC Stream<Res> interface with a vector instead of HTTP/2
// DATA frames. send() is a real coroutine; the dispatcher awaits it.

template <typename Res>
class MemStream final : public Stream<Res> {
public:
    Task<Result<void>> send(Res response) override {
        items.push_back(std::move(response));
        co_return Result<void>{};
    }
    [[nodiscard]] bool is_open() const noexcept override { return open; }

    std::vector<Res> items;
    bool             open{true};
};

// ─── In-memory AsyncChannel<Req> — replays client → server messages ───────────
// Implements the gRPC AsyncChannel<Req> interface. recv() returns each queued
// item then std::nullopt (client half-close). Never suspends — completes inline.

template <typename Req>
class MemChannel final : public AsyncChannel<Req> {
public:
    explicit MemChannel(std::vector<Req> q) : queue(std::move(q)) {}

    Task<Result<std::optional<Req>>> recv() override {
        if (pos < queue.size())
            co_return std::optional<Req>{ std::move(queue[pos++]) };
        co_return std::optional<Req>{ std::nullopt };  // EOS
    }

    std::vector<Req> queue;
    size_t           pos{0};
};

// ─── Synchronous coroutine driver ─────────────────────────────────────────────
// The dispatch_* coroutines here never suspend on real I/O — the handlers run
// inline and the in-memory Stream/Channel complete synchronously. A single
// resume() therefore drives the whole chain to completion. (Real network gRPC
// would instead drive these on a Reactor event loop.)

// Free-function driver coroutine (NOT a lambda): the coroutine frame owns the
// awaited task by value and references only run_sync's live `out`. A lambda
// coroutine here would dangle its closure (destroyed as a temporary while the
// frame still points at it) — UB that only manifests under optimization (-O3).
template <typename T>
static Task<void> drive_into(Task<T> task, T& out) {
    out = co_await std::move(task);
    co_return;
}

template <typename T>
static T run_sync(Task<T> task) {
    T result{};
    auto driver = drive_into<T>(std::move(task), result);
    while (driver.resume()) { /* synchronous chain — drive to completion */ }
    return result;
}

// ─── 1. Unary RPC: full deserialize → handle → serialize round-trip ───────────

static void demo_unary() {
    println("── 1. Unary RPC  (greeter.Greeter/SayHello) ──");

    GrpcHandler<HelloRequest, HelloReply> handler({
        .service_name = "greeter.Greeter",
        .method_name  = "SayHello",
    });

    // Plug in the (de)serializers — exactly where protobuf would go.
    handler.set_serializer(
        [](const HelloReply& r) {
            std::vector<uint8_t> b;
            put_string(b, r.message);
            return b;
        },
        [](std::span<const uint8_t> b) {
            return HelloRequest{ get_string(b) };
        });

    // The unary business logic — a real coroutine returning Result<HelloReply>.
    handler.set_unary([](HelloRequest req) -> Task<Result<HelloReply>> {
        co_return HelloReply{ "Hello, " + req.name + "!" };
    });

    assert(handler.has_unary());
    println("   grpc_path()    : {}", handler.grpc_path());

    // Inbound request envelope (as if decoded off an HTTP/2 DATA frame).
    GrpcMessage request = encode_request("qbuem");

    auto reply_msg = run_sync(handler.dispatch_unary(request));
    assert(reply_msg && "dispatch_unary failed");

    HelloReply reply{ get_string(reply_msg->payload) };
    println("   request.name   : qbuem");
    println("   reply.message  : {}", reply.message);
    assert(reply.message == "Hello, qbuem!");

    // gRPC trailers the server sends in the final HEADERS frame.
    std::string ok_trailer = handler.status_trailer(GrpcStatus::OK);
    assert(ok_trailer == "grpc-status: 0\r\n");
    println("   OK trailer     : grpc-status: 0");

    std::string err_trailer =
        handler.status_trailer(GrpcStatus::NOT_FOUND, "no such user");
    assert(err_trailer.find("grpc-status: 5") != std::string::npos);
    assert(err_trailer.find("grpc-message: no such user") != std::string::npos);
    println("   error trailer  : grpc-status: 5 / grpc-message: no such user");
    println();
}

// ─── 2. Length-Prefixed Message framing (the gRPC wire format) ────────────────

static void demo_framing() {
    println("── 2. gRPC Length-Prefixed framing (encode/decode round-trip) ──");

    using H = GrpcHandler<HelloRequest, HelloReply>;

    std::vector<uint8_t> payload;
    put_string(payload, "frame-me");
    GrpcMessage msg{ /*compressed=*/false, payload };

    // Encode → [flag:1][len:4 BE][payload]
    std::vector<uint8_t> wire = H::encode_message(msg);
    uint32_t framed_len =
        (uint32_t(wire[1]) << 24) | (uint32_t(wire[2]) << 16) |
        (uint32_t(wire[3]) <<  8) |  uint32_t(wire[4]);
    println("   payload bytes  : {}", payload.size());
    println("   wire bytes     : {}  (5-byte header + payload)", wire.size());
    println("   header flag    : 0x{:02x}  length field (BE): {}", wire[0], framed_len);
    assert(wire.size() == 5 + payload.size());
    assert(framed_len == payload.size());

    // Decode back and verify byte-for-byte equality.
    size_t consumed = 0;
    auto decoded = H::decode_message(std::span<const uint8_t>(wire), consumed);
    assert(decoded && "decode_message failed");
    assert(consumed == wire.size());
    assert(decoded->payload == payload);
    println("   consumed       : {} bytes  → round-trip payload match: {}",
            consumed, decoded->payload == payload ? "yes" : "NO");

    // A truncated buffer must report "need more data", never crash.
    size_t consumed2 = 0;
    auto partial = H::decode_message(std::span<const uint8_t>(wire).first(3), consumed2);
    assert(!partial && "expected error on short buffer");
    println("   short buffer   : decode returns error (need more), consumed={}", consumed2);
    println();
}

// ─── 3. Server-streaming RPC: one request → N responses ───────────────────────

static void demo_server_streaming() {
    println("── 3. Server-streaming RPC  (one request → N responses) ──");

    GrpcHandler<HelloRequest, HelloReply> handler({
        .service_name = "greeter.Greeter",
        .method_name  = "SayHelloStream",
    });
    handler.set_serializer(
        [](const HelloReply& r) { std::vector<uint8_t> b; put_string(b, r.message); return b; },
        [](std::span<const uint8_t> b) { return HelloRequest{ get_string(b) }; });

    handler.set_server_stream(
        [](HelloRequest req, Stream<HelloReply>& out) -> Task<void> {
            for (int i = 1; i <= 3 && out.is_open(); ++i) {
                auto r = co_await out.send(HelloReply{
                    std::format("ping {} for {}", i, req.name) });
                if (!r) co_return;  // client disconnected
            }
        });

    MemStream<HelloReply> stream;
    GrpcMessage request = encode_request("watcher");

    auto rc = run_sync(handler.dispatch_server_stream(request, stream));
    assert(rc && "dispatch_server_stream failed");

    for (auto& reply : stream.items) println("   << {}", reply.message);
    assert(stream.items.size() == 3);
    assert(stream.items[0].message == "ping 1 for watcher");
    assert(stream.items[2].message == "ping 3 for watcher");
    println();
}

// ─── 4. Client-streaming RPC: N requests → one response ───────────────────────

static void demo_client_streaming() {
    println("── 4. Client-streaming RPC  (N requests → one response) ──");

    GrpcHandler<HelloRequest, HelloReply> handler({
        .service_name = "greeter.Greeter",
        .method_name  = "CollectNames",
    });
    handler.set_serializer(
        [](const HelloReply& r) { std::vector<uint8_t> b; put_string(b, r.message); return b; },
        [](std::span<const uint8_t> b) { return HelloRequest{ get_string(b) }; });

    // Drain the request channel, then return a single aggregated reply.
    handler.set_client_stream(
        [](AsyncChannel<HelloRequest>& ch) -> Task<Result<HelloReply>> {
            std::string joined;
            int count = 0;
            while (true) {
                auto item = co_await ch.recv();
                if (!item) co_return std::unexpected(item.error());
                if (!item->has_value()) break;   // client half-close (EOS)
                if (count++) joined += ", ";
                joined += (*item)->name;
            }
            co_return HelloReply{ std::format("collected {}: {}", count, joined) };
        });

    auto channel = MemChannel<HelloRequest>(
        { HelloRequest{"alpha"}, HelloRequest{"bravo"}, HelloRequest{"charlie"} });

    auto reply_msg = run_sync(handler.dispatch_client_stream(channel));
    assert(reply_msg && "dispatch_client_stream failed");

    HelloReply reply{ get_string(reply_msg->payload) };
    println("   >> alpha, bravo, charlie  (then half-close)");
    println("   reply.message  : {}", reply.message);
    assert(reply.message == "collected 3: alpha, bravo, charlie");
    println();
}

// ─── 5. Bidirectional-streaming RPC: request stream ↔ response stream ─────────

static void demo_bidi_streaming() {
    println("── 5. Bidirectional-streaming RPC  (echo with [bidi] prefix) ──");

    GrpcHandler<HelloRequest, HelloReply> handler({
        .service_name = "greeter.Greeter",
        .method_name  = "Chat",
    });
    // Bidi needs no (de)serializer registered — dispatch_bidi works on the
    // already-decoded Req/Res objects flowing through the channel + stream.

    handler.set_bidi(
        [](AsyncChannel<HelloRequest>& in, Stream<HelloReply>& out) -> Task<void> {
            while (out.is_open()) {
                auto item = co_await in.recv();
                if (!item || !item->has_value()) break;  // error or EOS
                auto r = co_await out.send(HelloReply{ "[bidi] " + (*item)->name });
                if (!r) break;  // client gone
            }
        });

    auto channel = MemChannel<HelloRequest>(
        { HelloRequest{"alpha"}, HelloRequest{"bravo"}, HelloRequest{"charlie"} });
    MemStream<HelloReply> stream;

    auto rc = run_sync(handler.dispatch_bidi(channel, stream));
    assert(rc && "dispatch_bidi failed");

    for (auto& reply : stream.items) println("   <-> {}", reply.message);
    assert(stream.items.size() == 3);
    assert(stream.items[0].message == "[bidi] alpha");
    assert(stream.items[2].message == "[bidi] charlie");
    println();
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main() {
    println("╔══════════════════════════════════════════════════════════════╗");
    println("║  qbuem-stack — gRPC handler API (transport-independent core)   ║");
    println("╚══════════════════════════════════════════════════════════════╝");
    println();

    demo_unary();
    demo_framing();
    demo_server_streaming();
    demo_client_streaming();
    demo_bidi_streaming();

    println("All 5 gRPC streaming patterns ran in-process — assertions passed.");
    println("Note: the live HTTP/2 transport (Http2Client / Reactor) is the");
    println("      experimental layer and is intentionally not exercised here.");
    return 0;
}
