# cloud_gateway — ConvAI Cloud Gateway (WebSocket)

> Version: 0.1.0
> A self-contained, single-binary C++17 reference implementation of the
> ConvAI cloud gateway. Devices connect over **WebSocket** (RFC 6455,
> sub-protocol `convai.v1`) and exchange audio, events and function-call
> messages.

---

## Highlights

- **Pure C++17** — no external dependencies (no Boost, no OpenSSL, no WebSocket++).
- **WebSocket-only transport** — RFC 6455 server side, JSON envelopes for
  control, binary frames for audio.
- **epoll + thread pool** — single-threaded event loop for I/O, N workers
  for CPU-bound ASR/LLM/TTS.
- **Pluggable backends** — current build ships deterministic *stub*
  implementations of ASR / LLM / TTS so you can run the full pipeline
  end-to-end without external services.
- **Includes a C++ test client** (`gateway_test_client`) that performs
  the handshake, sends hello + audio, and prints every inbound frame.

---

## Quick start

```bash
# Requires: g++ (C++17), CMake >= 3.16, Linux/macOS (uses epoll)
cd cloud_gateway
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Terminal A: run the gateway (default port 9000)
./build/cloud_gateway --port 9000 --log-level debug

# Terminal B: drive a fake device
./build/gateway_test_client --host 127.0.0.1 --port 9000 --audio-frames 50
```

You should see handshake succeed, `hello_ack` arrive, audio frames
streamed in, then `status`, `function_call`, `text` and binary audio
frames flowing back from the stub backend.

---

## Command-line options

| Flag                     | Default     | Description                       |
|--------------------------|-------------|-----------------------------------|
| `--bind ADDR`            | `0.0.0.0`   | Bind address                      |
| `--port N`               | `9000`      | Listen port                       |
| `--workers N`            | `4`         | Thread-pool size                  |
| `--max-payload N`        | `65536`     | Max WS frame payload              |
| `--log-level LEVEL`      | `info`      | `debug` / `info` / `warn` / `error` |
| `--asr NAME` / `--llm` / `--tts` | `stub` | Currently only `stub` is supported |

Environment variable `GATEWAY_LOG_LEVEL` also accepted.

---

## Repository layout

```
cloud_gateway/
├── CMakeLists.txt
├── README.md            # this file
├── README_zh.md         # Chinese version
├── docs/cloud_gateway/  # full documentation
│   ├── index.md
│   ├── architecture.md
│   ├── protocol.md
│   ├── api_reference.md
│   └── deployment.md
├── include/cloud_gateway/
│   ├── log.hpp
│   ├── config.hpp
│   ├── protocol.hpp
│   └── session.hpp
├── src/
│   ├── main.cpp
│   ├── config.cpp
│   ├── protocol.cpp
│   ├── util/
│   │   ├── json_helper.{hpp,cpp}
│   │   ├── base64.{hpp,cpp}
│   │   └── sha1.{hpp,cpp}
│   ├── ws/
│   │   ├── frame.{hpp,cpp}      # RFC 6455 frame parser/serializer
│   │   ├── handshake.{hpp,cpp}  # HTTP upgrade + Sec-WebSocket-Accept
│   │   └── server.{hpp,cpp}     # epoll-based WS server
│   ├── codec/
│   │   └── envelope.{hpp,cpp}    # JSON envelopes & binary audio
│   ├── server/
│   │   └── thread_pool.{hpp,cpp}
│   ├── session/
│   │   ├── session.{hpp,cpp}
│   │   └── session_manager.{hpp,cpp}
│   └── upstream/
│       ├── asr_backend.{hpp,cpp}
│       ├── llm_backend.{hpp,cpp}
│       └── tts_backend.{hpp,cpp}
└── test/
    └── test_client.cpp
```

---

## Documentation

The full protocol spec, architecture diagrams and API reference live in
[`docs/cloud_gateway/`](docs/cloud_gateway/index.md).

| Doc | What's in it |
|---|---|
| [`index.md`](docs/cloud_gateway/index.md) | Overview & reading order |
| [`architecture.md`](docs/cloud_gateway/architecture.md) | Modules, threading, lifecycle |
| [`protocol.md`](docs/cloud_gateway/protocol.md) | Wire format, message types, sequence diagrams |
| [`api_reference.md`](docs/cloud_gateway/api_reference.md) | C++ classes and functions |
| [`deployment.md`](docs/cloud_gateway/deployment.md) | Build, run, harden for production |

---

## Compatibility with `ai-hardware-agent-examples`

The reference gateway implements the same message taxonomy used by the
device-side `sdk_integration/convai_bridge.c`. To exercise it from a real
device:

1. Set `convai_info_t` in `convai_bridge.c` to point at this gateway's
   `ws://host:port/` endpoint (currently the SDK goes through mbedtls
   + WebSocket; this gateway speaks plaintext WS for development).
2. The bridge's existing `convai_create / convai_start / convai_send_audio
   / on_convai_message_data` calls will be served by this gateway's
   session pipeline, exercising the function-call protocol with the
   `emotion` tool that `apps/settings` implements.

> See [`docs/cloud_gateway/protocol.md`](docs/cloud_gateway/protocol.md) §
> *Compatibility* for the full message-level mapping.

---

## License

Reference implementation; license inherited from the parent project.
For real deployment you should add TLS, authentication and rate
limiting — see `docs/cloud_gateway/deployment.md`.