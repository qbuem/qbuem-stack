/**
 * @file examples/04-codec-security/transport_codec/transport_codec_example.cpp
 * @brief Transport framing codecs — LengthPrefixedCodec + LineCodec.
 *
 * Teaching goals (the four pillars in action):
 *   1. LengthPrefixedCodec encode → wire bytes → incremental decode, showing the
 *      Incomplete → Complete state machine as a TCP-like stream is delivered in
 *      arbitrarily small chunks (header split, body split, multiple frames per
 *      chunk — every realistic boundary a real socket produces).
 *   2. LineCodec zero-copy line framing (CRLF for RESP/SMTP/HTTP, LF for logs).
 *      `Line::data` is a string_view straight into the receive buffer — no copy.
 *   3. The max-frame DoS guard: a 4-byte attacker-controlled length prefix that
 *      claims a multi-GiB body is rejected with DecodeStatus::Error *before* any
 *      allocation is attempted.
 *
 * Zero-copy / zero-alloc idioms used here:
 *   - encode() emits an iovec[2] scatter-gather pair (header + payload) — the
 *     payload vec points straight at the frame's bytes, no gather copy.
 *   - the decoder advances a BufferView (std::span<const uint8_t>) via subspan;
 *     no buffer is rebuilt per chunk.
 *   - LineCodec hands back string_views into the original buffer.
 *
 * Build standalone:
 *   clang++ -std=c++23 -O1 -I <repo>/include \
 *     transport_codec_example.cpp -o /tmp/transport_codec && /tmp/transport_codec
 */

#include <qbuem/codec/frame_codec.hpp>
#include <qbuem/codec/length_prefix_codec.hpp>
#include <qbuem/codec/line_codec.hpp>
#include <qbuem/compat/print.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <sys/uio.h>

using namespace qbuem::codec;
using qbuem::BufferView;
using std::println;

namespace {

// ─── helpers ──────────────────────────────────────────────────────────────────

constexpr std::string_view status_name(DecodeStatus s) {
  switch (s) {
    case DecodeStatus::Complete:   return "Complete";
    case DecodeStatus::Incomplete: return "Incomplete";
    case DecodeStatus::Error:      return "Error";
  }
  return "?";
}

// View a string literal/payload as raw bytes without copying.
std::span<const std::byte> as_bytes(std::string_view sv) {
  return {reinterpret_cast<const std::byte *>(sv.data()), sv.size()};
}

void banner(std::string_view title) {
  println("");
  println("┌────────────────────────────────────────────────────────────┐");
  println("│ {:<58} │", title);
  println("└────────────────────────────────────────────────────────────┘");
}

// ─── 1. Length-prefix encode → stream → incremental decode ──────────────────────

// A fixed, stack-resident wire buffer. No std::vector, no heap growth on the
// "send" path of the demo — encode() writes a 4-byte BE header + payload here.
// 4096 is comfortably larger than the demo messages.
struct WireBuffer {
  std::array<std::uint8_t, 4096> bytes{};
  std::size_t                    len = 0;

  // Encode one frame and append its iovec segments contiguously.
  void append_frame(LengthPrefixedCodec &codec, std::string_view payload) {
    LengthPrefixedFrame frame;
    auto pb = as_bytes(payload);
    frame.payload.assign(pb.begin(), pb.end());
    frame.length = static_cast<std::uint32_t>(payload.size());

    iovec vecs[2];
    std::size_t n = codec.encode(frame, vecs, 2, /*arena=*/nullptr);
    for (std::size_t i = 0; i < n; ++i) {
      const auto *p = static_cast<const std::uint8_t *>(vecs[i].iov_base);
      for (std::size_t b = 0; b < vecs[i].iov_len && len < bytes.size(); ++b) {
        bytes[len++] = p[b];
      }
    }
  }

  std::span<const std::uint8_t> view() const { return {bytes.data(), len}; }
};

void length_prefix_streaming_demo() {
  banner("1. LengthPrefixedCodec — frame, stream, decode incrementally");

  // (a) Frame three messages into one contiguous wire stream.
  // encode() is stateless w.r.t. framing, so a single codec instance is reused.
  LengthPrefixedCodec encoder;
  WireBuffer          wire;

  constexpr std::array<std::string_view, 3> messages{
      std::string_view{"ORDER:AAPL:100@189.42"},
      std::string_view{"ORDER:MSFT:50@402.10"},
      std::string_view{"CANCEL:order-7781"},
  };
  for (auto m : messages) wire.append_frame(encoder, m);

  std::size_t payload_total = 0;
  for (auto m : messages) payload_total += m.size();
  println("Framed {} messages → {} wire bytes ({} payload + {} header bytes).",
          messages.size(), wire.len, payload_total, messages.size() * 4);

  // (b) Receiver: feed the wire stream in deliberately ugly chunk sizes so the
  // parser must survive every boundary — header split mid-prefix, body split,
  // and two frames arriving inside one chunk.
  // Chunk plan (bytes): 2, 30, 7, then "everything remaining".
  constexpr std::array<std::size_t, 3> chunk_plan{2, 30, 7};

  LengthPrefixedCodec decoder;
  auto                 stream   = wire.view();
  std::size_t          offset   = 0;
  std::size_t          frames   = 0;
  std::size_t          chunk_no = 0;

  println("");
  println("Delivering the stream in chunks (a real socket never respects frame "
          "boundaries):");

  while (offset < stream.size()) {
    // Determine this chunk's size; last chunk takes the remainder.
    std::size_t want =
        (chunk_no < chunk_plan.size()) ? chunk_plan[chunk_no] : (stream.size() - offset);
    std::size_t take = std::min(want, stream.size() - offset);
    ++chunk_no;

    // A BufferView (std::span) over just-arrived bytes — zero copy.
    BufferView buf = stream.subspan(offset, take);
    offset += take;
    println("  chunk #{:<2} delivered {:>2} byte(s) [stream offset now {}/{}]",
            chunk_no, take, offset, stream.size());

    // Drain every complete frame currently decodable from `buf`. decode()
    // advances `buf` (via subspan) by the bytes it consumed, even on Incomplete.
    for (;;) {
      LengthPrefixedFrame frame;
      DecodeStatus        st = decoder.decode(buf, frame);

      if (st == DecodeStatus::Complete) {
        std::string_view text{reinterpret_cast<const char *>(frame.payload.data()),
                              frame.payload.size()};
        println("      → {:<10} frame #{} ({} bytes): \"{}\"", status_name(st),
                ++frames, frame.length, text);
        decoder.reset();  // ready for the next frame's header
        if (buf.empty()) break;  // nothing left in this chunk
        continue;                // another frame may be fully present
      }

      // Incomplete: parser kept the partial bytes internally; wait for more.
      println("      → {:<10} (parser is holding partial frame state, awaiting "
              "more bytes)",
              status_name(st));
      break;
    }
  }

  println("");
  println("Decoded {}/{} frames across {} chunks — payloads survived every split.",
          frames, messages.size(), chunk_no);
}

// ─── 2. Line framing (CRLF + LF), zero-copy ─────────────────────────────────────

void line_codec_demo() {
  banner("2. LineCodec — zero-copy line framing");

  // CRLF mode: Redis RESP / SMTP / HTTP request line + headers.
  // The whole request lives in one stack buffer; Line::data views into it.
  static constexpr std::string_view http_head =
      "GET /v1/orders HTTP/1.1\r\n"
      "Host: api.qbuem.com\r\n"
      "Accept: application/json\r\n"
      "\r\n";  // blank line terminates the header block

  println("CRLF mode (HTTP/RESP/SMTP) — splitting a request head into lines:");
  LineCodec crlf{/*crlf=*/true};
  BufferView buf{reinterpret_cast<const std::uint8_t *>(http_head.data()),
                 http_head.size()};

  std::size_t line_no = 0;
  for (;;) {
    Line         line;
    DecodeStatus st = crlf.decode(buf, line);
    if (st != DecodeStatus::Complete) break;  // Incomplete = no more full lines
    crlf.reset();
    ++line_no;
    if (line.data.empty()) {
      println("  line {}: <blank> → end of header block", line_no);
      break;
    }
    // line.data is a string_view straight into http_head — no allocation/copy.
    println("  line {}: \"{}\"  (view ptr inside source buffer = {})", line_no,
            line.data,
            static_cast<const void *>(line.data.data()) >= http_head.data() &&
                    static_cast<const void *>(line.data.data()) <
                        http_head.data() + http_head.size()
                ? "yes"
                : "no");
  }

  // LF mode: plain log streams.
  static constexpr std::string_view log_stream =
      "INFO  reactor started on core 3\n"
      "WARN  shm ring 82% full\n"
      "ERROR backpressure: dropping low-priority frame\n";

  println("");
  println("LF mode (log streams) — one line == one frame:");
  LineCodec lf{/*crlf=*/false};
  BufferView log_buf{reinterpret_cast<const std::uint8_t *>(log_stream.data()),
                     log_stream.size()};
  std::size_t logs = 0;
  for (;;) {
    Line         line;
    DecodeStatus st = lf.decode(log_buf, line);
    if (st != DecodeStatus::Complete) break;
    lf.reset();
    println("  log[{}]: {}", logs++, line.data);
  }
  println("Decoded {} log lines (each Line::data is a zero-copy view).", logs);
}

// ─── 3. Max-frame DoS guard ─────────────────────────────────────────────────────

void dos_guard_demo() {
  banner("3. Max-frame DoS guard — reject an oversized length prefix");

  // An attacker sends a valid-looking 4-byte big-endian prefix claiming a body
  // of 0xFFFFFFFF (~4 GiB). The codec's internal cap is 64 MiB. Without the
  // guard, decode() would try to reserve ~4 GiB → OOM / process kill.
  // The guard returns DecodeStatus::Error BEFORE any allocation.
  constexpr std::uint32_t claimed = 0xFFFFFFFFu;  // ~4 GiB
  std::array<std::uint8_t, 4> malicious{
      static_cast<std::uint8_t>((claimed >> 24) & 0xFF),  // big-endian
      static_cast<std::uint8_t>((claimed >> 16) & 0xFF),
      static_cast<std::uint8_t>((claimed >> 8) & 0xFF),
      static_cast<std::uint8_t>(claimed & 0xFF),
  };

  println("Attacker prefix (big-endian): {:02X} {:02X} {:02X} {:02X}  → claims "
          "{} bytes (~{:.2f} GiB)",
          malicious[0], malicious[1], malicious[2], malicious[3], claimed,
          static_cast<double>(claimed) / (1024.0 * 1024.0 * 1024.0));
  println("Codec hard cap: 64 MiB. Decoding the prefix...");

  LengthPrefixedCodec   codec;
  BufferView            buf{malicious.data(), malicious.size()};
  LengthPrefixedFrame   frame;
  DecodeStatus          st = codec.decode(buf, frame);

  println("  → decode() returned: {}", status_name(st));
  if (st == DecodeStatus::Error) {
    println("  ✓ guard fired: oversized frame rejected with NO allocation. "
            "Caller should close the connection.");
  } else {
    println("  ✗ UNEXPECTED: guard did not fire (this would be a bug).");
  }

  // Contrast: a legitimately-sized prefix is accepted (here it just goes
  // Incomplete because the body bytes haven't arrived yet — proving the guard
  // only rejects oversize, never well-formed frames).
  constexpr std::uint32_t ok_len = 32u;
  std::array<std::uint8_t, 4> honest{
      static_cast<std::uint8_t>((ok_len >> 24) & 0xFF),
      static_cast<std::uint8_t>((ok_len >> 16) & 0xFF),
      static_cast<std::uint8_t>((ok_len >> 8) & 0xFF),
      static_cast<std::uint8_t>(ok_len & 0xFF),
  };
  LengthPrefixedCodec ok_codec;
  BufferView          ok_buf{honest.data(), honest.size()};
  LengthPrefixedFrame ok_frame;
  DecodeStatus        ok_st = ok_codec.decode(ok_buf, ok_frame);
  println("");
  println("Contrast — honest prefix claiming {} bytes (header only delivered): {}",
          ok_len, status_name(ok_st));
  println("  ✓ well-formed frame accepted (Incomplete = waiting for the {} "
          "body bytes, not rejected).",
          ok_len);
}

}  // namespace

int main() {
  println("============================================================");
  println(" qbuem-stack · Transport Framing Codecs");
  println(" LengthPrefixedCodec (4B BE prefix) + LineCodec (CRLF/LF)");
  println("============================================================");

  length_prefix_streaming_demo();
  line_codec_demo();
  dos_guard_demo();

  println("");
  println("All demos complete.");
  return 0;
}
