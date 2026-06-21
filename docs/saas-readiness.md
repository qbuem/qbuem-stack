# SaaS Readiness — Architecture & Plan

> Status: **active program** (started 2026-06-21). This document records the
> decisions and the phased plan for making qbuem-stack usable as a general web /
> SaaS server, without abandoning the zero-dependency contract. It complements
> `roadmap.md` (which tracks shipped state) — this is the forward plan.

## 1. Decision: serve general web / SaaS

The library was dogfooded against a real-time game server (`qbuem-game`), and its
deep, tested surface reflects that. The product direction now also requires a
**general web service / SaaS server**. The previously "codec-only" surfaces
(HTTP/2, gRPC) and the "interface-only" DB layer move from *candidates for
removal* to *the core of the SaaS build-out*.

## 2. Locked architecture decisions

| # | Decision | Consequence |
|---|----------|-------------|
| **A** | **Dependencies via abstraction (ports & adapters).** The core (`include/qbuem/`) stays strictly zero-dependency and defines **ports** (abstract interfaces) for anything that would need a third party. Concrete **adapters** (OpenSSL, libpq, OTLP, …) live in separate **opt-in** targets/modules and are the *only* place a third-party header appears. The core never includes one. | Pillar 4 (Zero Dependency) is preserved for the core; SaaS users opt into adapters via `QBUEM_WITH_*` CMake flags. Dependency inversion: core depends on interfaces, never on concretes. |
| **B** | **TLS terminated at the edge.** A reverse proxy / load balancer / CDN (nginx, ALB, Cloudflare) terminates TLS; the app speaks cleartext **HTTP/1.1 + h2c** behind it. | No in-process TLS in the core. HTTP/2 is implemented over **h2c** (prior-knowledge preface or `Upgrade: h2c`) — **no ALPN, no in-process TLS library**. An optional in-process TLS adapter (decision A) can come later for standalone-binary deployments. |
| **C** | **DB stays interface-only (BYO driver).** The `db` layer remains a port (`IConnection` / `IConnectionPool`); no bundled native driver. | We invest in making the *port* good (async-friendly, pool-backed) and document the adapter pattern; a reference adapter ships as an **opt-in example**, not in core. |

## 3. Gap analysis — what a SaaS server needs

| Capability | Before | Target | 안건 |
|---|---|---|---|
| TLS/HTTPS | none (Pillar 4) | edge-terminated; app is h2c/http1 plaintext | 1 |
| HTTP/2 server | codec-only (no flow control, no SETTINGS negotiation, no dynamic HPACK, no socket loop) | real **h2c** server on the App umbrella | 1 |
| gRPC server | codec-only (no transport) | gRPC over the h2c server | 1 |
| DB persistence | interface + mock | polished port + reference adapter (opt-in) | 1 |
| AuthN/Z | HS256 JWT verify | + RS256/JWKS (OIDC) via a verifier **port**; tenant scoping | 1 |
| Observability export | stderr/in-memory only | OTLP/file exporter **adapter** (opt-in) behind the existing `SpanExporter` port | 1/3 |
| Rate/quota | global rate-limit middleware | per-tenant quota middleware | 1 |
| Pipeline surface | 4 builders, 3 windowings, dup debounce/throttle, 0-consumer SLO observers | lean, deduplicated; deprecate then remove the redundant tail | 2 |
| Test depth | many construct-only tests | behavioral + concurrency/load tests on the SaaS path | 3 |

**Key framing:** behind an edge TLS terminator, the existing **Stable HTTP/1.1 App
+ middleware** path already covers a SaaS MVP. The build-out fills h2c, DB,
RS256/JWKS, OTLP, and multi-tenancy — not a from-scratch effort.

## 4. Phased plan

### 안건 1 — SaaS surfaces (build out)

**Priority (locked 2026-06-21 by product answers): SaaS critical path over the
existing Stable HTTP/1.1 App first; HTTP/2/gRPC deferred.** Rationale: with TLS
terminated at the edge, the proxy speaks h2 to clients and the backend can speak
HTTP/1.1 — so h2c-to-backend is a performance nicety, not a launch blocker. The
product is REST/JSON (no gRPC needed yet) and auth uses an external IdP (RS256).

Critical path (do first):
1. **Auth — RS256/JWKS** (external-IdP tokens). Sub-steps:
   - a. **Native RS256 verify primitive** ✅ *(done — `crypto/rsa.hpp`; see §5)*
   - b. **JWK / JWKS parsing** (kid → modulus/exponent) ✅ *(done — `parse_jwks`)*
   - c. **`Rs256JwtVerifier`** implementing the existing `ITokenVerifier` port
     (alg=RS256 pin + kid lookup + `crypto::rsa_pkcs1_v15_sha256_verify` + exp/nbf),
     usable directly with `bearer_auth()` ✅ *(done — `middleware/rs256_verifier.hpp`;
     see §5)*. (HS256 stays native; an OpenSSL adapter could replace the RSA step
     behind the same port if hardware RSA is ever wanted.)
   - d. *(optional)* JWKS HTTPS **fetch adapter** (opt-in) + key-set cache/rotation
     — until then the JWKS document is supplied out-of-band / from config.
2. **Multi-tenancy**: tenant context + per-tenant **quota** middleware (extends
   the existing rate-limit middleware).
3. **DB port polish** + one reference adapter as an opt-in example.
4. **OTLP exporter adapter** behind the existing `SpanExporter` port (opt-in).

Deferred until a concrete need (gRPC service, or backend h2 multiplexing):
5. HTTP/2 **dynamic HPACK table** (encoder + decoder) with size accounting.
6. HTTP/2 **h2c socket loop**: connection preface, `Upgrade: h2c` from the App
   umbrella, frame read/write loop on the reactor, send-window **back-pressure**.
   *(The SETTINGS negotiation + flow control from step 0 below is the foundation
   already in place for this.)*
7. **gRPC over h2c**: map `GrpcHandler` framing onto the h2c server.

Already done (foundation, kept regardless of ordering):
- 0. **HTTP/2 SETTINGS negotiation + flow control** ✅ *(`http2_handler.hpp`; §5)*

### 안건 2 — pipeline de-duplication (depth not breadth)
- Remove confirmed zero-consumer surface (e.g. `SloObserver` hooks).
- **Deprecate** (not delete yet — `qbuem-game` pins a tag) the redundant tail:
  `PipelineFactory`, `migration`, one of the duplicate windowing/debounce/throttle
  implementations, `priority_channel`. Schedule removal for v2.0.

### 안건 3 — test depth
- Promote construct-only tests to behavioral: `RetryAction`, `ScatterGatherAction`,
  `CheckpointedPipeline`, `IdempotencyFilter`.
- Add concurrency/load tests on the SaaS request path (h2c).

## 5. Progress log

- **2026-06-21** — 안건 1.1.b/c: **RS256 JWT + JWKS verifier**
  (`include/qbuem/middleware/rs256_verifier.hpp`). `parse_jwks()` turns a JWKS (or
  a bare JWK) document into RSA public keys (base64url n/e → bytes; brace-scan over
  the `keys` array). `Rs256JwtVerifier` implements the existing `ITokenVerifier`
  port — splits the JWT, **pins `alg` to RS256** (rejecting `none`/`HS256`
  downgrade), selects the key by `kid`, verifies the signature via
  `crypto::rsa_pkcs1_v15_sha256_verify` over the signing input, then enforces
  `exp`/`nbf` and extracts `sub`/`iss`/`aud` — so it drops straight into
  `bearer_auth()`. This completes **end-to-end external-IdP (RS256) token auth** on
  the existing HTTP/1.1 App. Tests: `tests/rs256_jwt_test.cpp` — 8 tests against an
  OpenSSL-minted JWT+JWKS (valid; tampered/expired/alg-none/alg-HS256/wrong-key/
  malformed rejected); Release + ASan/UBSan clean. Next: 안건 2 — per-tenant quota.
- **2026-06-21** — 안건 1.1.a: native **RS256 verify** (`include/qbuem/crypto/rsa.hpp`).
  RSASSA-PKCS1-v1.5 + SHA-256 signature *verification*, zero-dependency: a compact
  verify-only big-integer (no division/Montgomery — modular reduction by binary
  long division; square-and-multiply modexp) does `s^e mod n`, then EMSA-PKCS1-v1.5
  padding + SHA-256 DigestInfo are checked in constant time. Verification only (no
  keygen/sign/private-key). Foundation for RS256/JWKS IdP-token auth — and, per the
  ports-and-adapters layout, replaceable by an OpenSSL/mbedTLS adapter behind the
  `ITokenVerifier` port. Tests: `tests/rsa_verify_test.cpp` — 6 tests against an
  OpenSSL-minted RSA-2048 vector (valid + tamper/wrong-msg/wrong-exp/wrong-len/
  leading-zero-modulus); Release + ASan/UBSan clean. Next: 1.1.b/c — JWK(S) parse +
  `Rs256JwtVerifier` (`ITokenVerifier`) wired into `token_auth`.
- **2026-06-21** — 안건 1.0: HTTP/2 **SETTINGS negotiation + flow control**
  (`include/qbuem/server/http2_handler.hpp`). Real peer-SETTINGS parsing/validation
  (`INITIAL_WINDOW_SIZE` with §6.9.2 retroactive stream-window adjustment,
  `MAX_FRAME_SIZE` range check, `ENABLE_PUSH` 0/1, ACK rules); connection +
  per-stream send/receive windows; `WINDOW_UPDATE` handling (overflow → GOAWAY/RST,
  zero increment → error); receive-side enforcement with auto-replenishment;
  send-side accounting (back-pressure enforcement deferred to 1.3, the socket loop).
  Tests: `tests/http2_flow_control_test.cpp` (9 behavioral tests; Release +
  ASan/UBSan clean). The handler is still not wired to a socket — that is step 1.3.
