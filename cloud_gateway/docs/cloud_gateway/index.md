# cloud_gateway Documentation

Welcome to the **cloud_gateway** reference implementation. This gateway
is the cloud-side counterpart to the device-side ConvAI SDK in the
parent project (`ai-hardware-agent-examples`). It is a single C++17
binary that accepts WebSocket connections from devices, runs a
deterministic audio → ASR → LLM → TTS pipeline, and dispatches function
calls back to the device (e.g. emotion selection).

---

## Read in this order

1. **[Architecture](architecture.md)** — modules, threading model, lifecycle
   diagrams, data flow.
2. **[Protocol](protocol.md)** — the wire format, message types, sample
   sequences, and how the device-side SDK maps to it.
3. **[API reference](api_reference.md)** — every public class / function
   in `include/cloud_gateway/*.hpp` and the internal interfaces.
4. **[Deployment](deployment.md)** — build, run, harden for production,
   troubleshooting.

---

## What you get

- `cloud_gateway` binary — the server (`--port 9000`)
- `gateway_test_client` binary — a minimal C++ WebSocket client to drive
  the server from your terminal
- Stub ASR / LLM / TTS — deterministic, no external service needed
- Plug-in points to swap in real backends (e.g. iFlytek / Whisper /
  GPT-4o / Volcano TTS) — keep the same callback signatures

---

## Quick taste of the protocol

Devices open a WebSocket to the gateway with:

```
GET / HTTP/1.1
Host: gateway:9000
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Key: <base64 16 bytes>
Sec-WebSocket-Version: 13
Sec-WebSocket-Protocol: convai.v1
```

After the 101 Switching Protocols response, two message classes share
the connection:

- **TEXT** frames carry JSON envelopes:
  ```json
  {"type":"hello","seq":1,"ts":1737034800,"body":{"product_id":"..."}}
  ```
- **BINARY** frames carry G.711A audio with a 13-byte mini-header:
  ```
  | AudioOp (1B) | Sequence (4B BE) | Timestamp (8B BE) | G.711A bytes |
  ```

The full taxonomy is in [`protocol.md`](protocol.md).

---

## Project at a glance

| | |
|---|---|
| Language   | C++17 |
| Build      | CMake ≥ 3.16 |
| Runtime    | Linux / macOS (uses epoll) |
| Dependencies | None (header-only JSON / SHA1 / Base64 in-tree) |
| Lines of code | ~3,500 (C++ + headers + docs) |

See [Deployment → Build](deployment.md#build) for exact recipes.