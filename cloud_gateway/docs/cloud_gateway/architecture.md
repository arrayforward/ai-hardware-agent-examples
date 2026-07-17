# Architecture

This document describes how `cloud_gateway` is organised internally.
It complements the source comments and the [API reference](api_reference.md).

---

## Layered view

```
┌─────────────────────────────────────────────────────────────┐
│                          main.cpp                            │
│  signal handling · Config · WsServer.run · shutdown coordination│
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│  ws::Server       (single epoll loop, runs forever)          │
│   ├─ listen_fd     → accept loop                            │
│   └─ per-fd WsConnection state machine:                     │
│         HANDSHAKING → OPEN → CLOSING → CLOSED              │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼  (frames after handshake)
┌─────────────────────────────────────────────────────────────┐
│  Session          (one per device, in main map)             │
│   ├─ parse envelope  (msg type dispatch)                    │
│   ├─ route to ASR / LLM / TTS via ThreadPool               │
│   └─ send outbound (text + binary)                           │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│  Upstream backends (interfaces)                              │
│   AsrBackend · LlmBackend · TtsBackend                        │
│   stub impls in-tree; production impls plug in here          │
└─────────────────────────────────────────────────────────────┘
```

---

## Threading model

| Thread                | What it does                                            | Blocking? |
|-----------------------|--------------------------------------------------------|-----------|
| `main`                | Sets up signal handlers, starts `WsServer.run()`       | yes       |
| io thread (epoll)     | `epoll_wait` loop, read/write every connection, dispatch frame callbacks | no (uses non-blocking I/O) |
| worker pool (N)       | ASR / LLM / TTS stub computations                       | CPU only, no I/O |
| watchdog              | Wakes every 200 ms to break the epoll loop on SIGINT    | yes       |

- All session-level callbacks (`Session::on_hello`, `on_audio_binary`,
  etc.) are invoked **from the io thread**.
- All upstream backends are called **from a worker thread** via
  `ThreadPool::submit`. Callbacks back into `Session` always run on a
  worker thread; `Session::send_*` writes to the connection's write
  queue which is drained by the io thread on `EPOLLOUT`.

This means:

- Never call `send_*` from inside a worker callback without thinking
  twice — it just enqueues, so it's safe but it ties up a worker.
- Never block the io thread. CPU-bound work always goes to the pool.

---

## Per-connection state machine

```
                 ┌──────────────┐
                 │  ACCEPTED    │  new fd, EPOLLIN
                 └──────┬───────┘
                        │ first read → parse HTTP upgrade
                        ▼
                 ┌──────────────┐
                 │ HANDSHAKING  │  parse_handshake()
                 └──────┬───────┘
                        │ 101 Switching Protocols sent
                        ▼
                 ┌──────────────┐
                 │     OPEN     │  WsFrameParser on every read
                 └──────┬───────┘
                        │ CLOSE frame OR peer hangup
                        ▼
                 ┌──────────────┐
                 │   CLOSING    │  send CLOSE frame
                 └──────┬───────┘
                        │ write drained (or hard error)
                        ▼
                 ┌──────────────┐
                 │   CLOSED     │  epoll_ctl(DEL) + erase from map
                 └──────────────┘
```

Transitions are driven by `WsConnection::on_readable()` and
`on_writable()`, both called from the io thread.

---

## Session pipeline

When a `BINARY AUDIO_FRAME` arrives, the session does the following
asynchronously:

```
                   20ms G.711A
                        │
                        ▼
                ThreadPool::submit
                        │
                        ▼
                  StubAsr::feed
                        │
                  transcript
                        ▼
                 session status = THINKING
                        │
                        ▼
                ThreadPool::submit
                        │
                        ▼
                  StubLlm::chat
                        │
                  chunks [{text, ...}, {function_call, ...}]
                        │
        ┌───────────────┼───────────────┐
        ▼               ▼               ▼
   send TEXT       send FC       session status = ANSWERING
                                          │
                                          ▼
                                  ThreadPool::submit
                                          │
                                          ▼
                                    StubTts::synth
                                          │
                                  AudioFrames [{op=Frame, seq, pcm}]
                                          │
                                          ▼
                       send Start → Frames → End
                                          │
                                          ▼
                          session status = ANSWER_FINISHED → LISTENING
```

Every boxed step is a worker-pool task. The io thread only does the
network I/O; CPU work never blocks it.

---

## Module map

| File | Purpose |
|---|---|
| `src/main.cpp` | Entry point, signal handling, wiring |
| `src/config.cpp` | CLI parsing (`getopt_long`) |
| `src/protocol.cpp` | `MsgType` ↔ string conversions |
| `src/util/json_helper.{hpp,cpp}` | Self-contained JSON encoder/decoder |
| `src/util/base64.{hpp,cpp}` | RFC 4648 base64 (for WS handshake) |
| `src/util/sha1.{hpp,cpp}` | RFC 3174 SHA-1 (for WS handshake) |
| `src/ws/frame.{hpp,cpp}` | RFC 6455 frame parser & serializer |
| `src/ws/handshake.{hpp,cpp}` | HTTP upgrade parser & 101-response builder |
| `src/ws/server.{hpp,cpp}` | epoll loop + `WsConnection` |
| `src/codec/envelope.{hpp,cpp}` | Text envelopes and binary audio frames |
| `src/server/thread_pool.{hpp,cpp}` | Fixed-size worker pool with `submit()` |
| `src/session/session.{hpp,cpp}` | Per-connection session logic |
| `src/session/session_manager.{hpp,cpp}` | Session map |
| `src/upstream/asr_backend.{hpp,cpp}` | ASR interface + stub |
| `src/upstream/llm_backend.{hpp,cpp}` | LLM interface + stub |
| `src/upstream/tts_backend.{hpp,cpp}` | TTS interface + stub |

---

## Why not (yet) TLS?

The reference build is **plaintext WebSocket**. Production deployment
should put the gateway behind a TLS terminator (e.g. nginx with
`stream { ... }` or `stunnel`) — see
[Deployment → Hardening](deployment.md#hardening).

The SDK on the device (`platform/convai_platform_ws63.c:215`) currently
uses `mbedtls_net_connect(..., MBEDTLS_NET_PROTO_TCP)` and the device
assumes WebSocket over TLS. To exercise this gateway from a real WS63
board you have two options:

1. Use a TLS-terminating reverse proxy in front of the gateway
2. Extend `ws/server.cpp` to do TLS directly (with OpenSSL or mbedtls)

This is left to deployment-time customisation; the plaintext path keeps
the reference implementation self-contained.

---

## Why not Boost / WebSocket++?

For a reference implementation the priorities are:

1. **No external dependencies** so anyone can clone & build
2. **Code that fits in your head** — every line can be read
3. **Single-file modules** so each layer is replaceable

The current RFC 6455 parser/serializer is ~150 lines. The HTTP upgrade
parser is ~80 lines. Both are short enough to audit; using a 10k-line
library would defeat the educational goal.

That said, the `WsServer` is deliberately written so that the underlying
transport could be swapped (e.g. for Boost.Beast) without touching
session logic. See `WsConnection::send_*` and `WsConnection::on_readable`
as the seams.