# codec

**Category:** Codec & Security
**File:** `codec_example.cpp`
**Complexity:** Intermediate

## Overview

Demonstrates two protocol codecs built into qbuem-stack:

- **`LengthPrefixedCodec`** — 4-byte big-endian length header + variable payload. Used in custom binary protocols, RPC, and message queues.
- **`LineCodec`** — CRLF or LF line-delimited text. Used in HTTP/1.1 headers, Redis RESP, and log streaming.

## Scenario

A microservice communicates with a custom backend via a length-prefixed binary protocol and also parses incoming HTTP/1.1 request lines from a raw TCP stream. Both codecs handle partial reads (streaming scenarios where data arrives in fragments).

## Architecture Diagram

```
  LengthPrefixedCodec (encode)
  ──────────────────────────────────────────────────────────
  Message string "Hello Protocol!"
       │
       ▼
  encode(frame, iovec[2])
  ┌────────────┬──────────────────────┐
  │ [0] 4 B    │ [1] 15 B             │
  │ big-endian │ payload bytes        │
  │ length=15  │ "Hello Protocol!"    │
  └────────────┴──────────────────────┘
       │  wire bytes (19 total)
       ▼
  decode(BufferView) → DecodeStatus::Complete
  └─ frame.payload == "Hello Protocol!"

  LengthPrefixedCodec (partial read)
  ──────────────────────────────────────────────────────────
  First call:  decode(first 3 bytes)  → Incomplete
  Second call: decode(remaining bytes) → Complete

  LineCodec (CRLF mode)
  ──────────────────────────────────────────────────────────
  Raw buffer: "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n"
       │
       ▼  repeated decode() calls
  Line 1: "GET / HTTP/1.1"
  Line 2: "Host: example.com"
  Line 3: ""  ← empty line = end of headers

  LineCodec (LF mode)
  ──────────────────────────────────────────────────────────
  Raw buffer: "INFO: ...\nWARN: ...\nERROR: ...\n"
  → 3 log lines decoded
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `LengthPrefixedCodec` | 4-byte header framing codec |
| `codec.encode(frame, iovec[], n, nullptr)` | Encode frame into scatter-gather iovecs |
| `codec.decode(BufferView, frame)` | Parse bytes into frame; returns `DecodeStatus` |
| `codec.reset()` | Reset internal state for next frame |
| `DecodeStatus::Complete` | Full frame received |
| `DecodeStatus::Incomplete` | Waiting for more bytes |
| `LineCodec(crlf)` | CRLF (`true`) or LF-only (`false`) line codec |
| `line_codec.decode(view, line)` | Parse one line from the buffer |

## Input / Output

### LengthPrefixedCodec

| Step | Input | Output |
|------|-------|--------|
| `encode` | `"Hello Protocol!"` (15 B) | 19-byte wire buffer (4 B length + 15 B payload) |
| `decode` (full) | 19 bytes | `DecodeStatus::Complete`, payload = `"Hello Protocol!"` |
| `decode` (partial) | 3 bytes | `DecodeStatus::Incomplete` |
| `decode` (rest) | 16 bytes | `DecodeStatus::Complete` |

### LineCodec

| Mode | Input | Output |
|------|-------|--------|
| CRLF | HTTP request headers | 3 lines decoded |
| LF | Unix log lines | 3 log lines decoded |

## How to Run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target codec_example
./build/examples/04-codec-security/codec/codec_example
```

## Expected Output

```
=== LengthPrefixedCodec ===
[length_prefix] Encoded 19 bytes (4 header + 15 payload)
[length_prefix] Decoded: 'Hello Protocol!'
[length_prefix] Partial (3 bytes): Incomplete
[length_prefix] After rest: Complete

=== LineCodec (CRLF) ===
[line] Line 1: 'GET / HTTP/1.1'
[line] Line 2: 'Host: example.com'
[line] Line 3: ''
[line] Total lines decoded: 3

=== LineCodec (LF-only) ===
[lf_line] INFO: server started
[lf_line] WARN: high memory
[lf_line] ERROR: timeout
[lf_line] Total: 3 log lines
```

## Notes

- Call `codec.reset()` between frames to clear internal accumulation state.
- The codec is zero-copy on the encode path — it produces `iovec` slices pointing to the original data.
- For HTTP/1.1 parsing at the semantic level, use `qbuem::http::Parser` instead.
