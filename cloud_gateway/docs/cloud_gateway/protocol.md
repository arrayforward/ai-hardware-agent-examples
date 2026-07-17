# Wire Protocol v1 (`convai.v1`)

This document specifies the wire format between device and gateway.
The gateway speaks **RFC 6455 WebSocket** with sub-protocol
`convai.v1`. Two message classes share a single WS connection.

---

## Transport

- TCP, plaintext (use TLS reverse-proxy in production)
- Single long-lived connection per device
- Sub-protocol: `Sec-WebSocket-Protocol: convai.v1`
- Server endpoints accept any path; reserved for future multi-tenant
  routing (`/v1/...`)

### Handshake

Device sends a standard RFC 6455 opening handshake:

```
GET / HTTP/1.1
Host: gateway:9000
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Key: <base64-encoded 16 random bytes>
Sec-WebSocket-Version: 13
Sec-WebSocket-Protocol: convai.v1
```

Server replies:

```
HTTP/1.1 101 Switching Protocols
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Accept: <base64(sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))>
Sec-WebSocket-Protocol: convai.v1
Server: cloud_gateway/0.1
```

Server implementation: [`src/ws/handshake.cpp`](../../src/ws/handshake.cpp).

---

## Message classes

### Class 1 — Text envelopes (WebSocket opcode 0x1)

Single UTF-8 JSON object with this shape:

```json
{
  "type": "<string from MsgType>",
  "seq":  <uint32>,         // monotonically increasing per direction
  "ts":   <uint64>,         // ms since unix epoch
  "body": { ... }           // type-specific payload
}
```

`seq` and `ts` are advisory (used for diagnostics). Server implementations
**should** validate `type` and ignore unknown values rather than tearing
down the connection.

### Class 2 — Binary audio (WebSocket opcode 0x2)

13-byte mini-header followed by G.711A bytes:

| Offset | Type    | Field     |
|--------|---------|-----------|
| 0      | u8      | `AudioOp` |
| 1      | u32 BE  | `Sequence`|
| 5      | u64 BE  | `Timestamp` (ms) |
| 13     | u8[]    | G.711A samples |

`AudioOp` values:

| Hex | Name  | Meaning |
|-----|-------|---------|
| 0x10 | Frame  | 20 ms G.711A mono audio (160 bytes at 8 kHz) |
| 0x11 | Start  | VAD speech start (followed by Frames until End) |
| 0x12 | End    | VAD speech end (trigger ASR + LLM + TTS) |
| 0x13 | Cancel | User barge-in (drop in-flight buffers, stop TTS) |

---

## Message type catalogue

Direction: **D→G** = device to gateway, **G→D** = gateway to device.

| Type (string)          | Dir | Description |
|------------------------|-----|-------------|
| `hello`                | D→G | Device authentication & capabilities |
| `hello_ack`            | G→D | Session created |
| `hello_err`            | G→D | Authentication rejected (close follows) |
| `bye`                  | D→G | Polite close |
| `ping` / `pong`        | both| Optional WS-level keepalive (gateway also sends its own WS PING) |
| `status`               | G→D | Agent state change (matches `convai_status_e`) |
| `event`                | G→D | Lifecycle event (matches `convai_event_code_e`) |
| `text`                 | G→D | Complete AI reply text |
| `text_delta`           | G→D | Streaming text fragment |
| `config_update`        | D→G | Replace system messages / TTS voice |
| `config_update_ack`    | G→D | Accepted (gateways may apply lazily) |
| `config_update_err`    | G→D | Validation failed |
| `function_call`        | G→D | Cloud LLM tool invocation (e.g. `emotion`) |
| `function_call_output` | D→G | Device-side tool result (must be returned per FC) |
| `error`                | G→D | Generic error (carries `code` + `message`) |
| `ack`                  | G→D | Generic acknowledgement |

---

## Payload schemas

### `hello`

```json
{
  "product_id":     "your_product_id",
  "product_key":    "your_product_key",
  "product_secret": "your_product_secret",
  "device_name":    "ws63-001",
  "audio_codec":    1,            // 0=PCM16, 1=G.711A
  "sample_rate":    8000
}
```

### `hello_ack`

```json
{
  "session_id":   "sess_abc123...",
  "server_time":  1737034800123,
  "audio_config": {
    "frame_ms": 20,
    "codec":    "g711a",
    "vad":      "server"        // server-side VAD required
  }
}
```

### `hello_err`

```json
{ "code": "AUTH_FAILED", "message": "invalid product_key" }
```

### `status`

```json
{ "status": "listening" }   // idle|listening|thinking|answering|interrupted|answer_finished
```

### `event`

```json
{ "event": "connected", "details": "session established" }
```

### `config_update`

Mirrors the JSON produced by `apps/settings/main_app.cpp::generate_convai_config_json`:

```json
{
  "config": {
    "llm_config": {
      "system_messages": [
        "你的名字叫小荷…",
        "你是一个温柔又治愈的小伙伴…",
        "你现在扮演小朋友的暖心大姐姐…"
      ]
    },
    "tts_config": {
      "provider_params": {
        "audio": { "voice_type": "Chinese (Mandarin)_Warm_Girl" }
      }
    }
  }
}
```

### `config_update_ack`

```json
{ "result": "ok", "applied_at": 1737034812123 }
```

### `config_update_err`

```json
{ "code": "INVALID_VOICE", "message": "voice_type X not in catalogue" }
```

### `function_call`

Standard OpenAI Realtime-style payload. The device MUST respond with a
matching `function_call_output` for every call.

```json
{
  "type": "response.function_call_arguments.done",
  "event_id": "evt_xxx",
  "calls": [
    {
      "call_id":  "call_abc123",
      "name":     "emotion",
      "arguments": "{\"emotion\":\"happy\"}"
    }
  ]
}
```

### `function_call_output`

```json
{
  "type": "conversation.items.create",
  "items": [
    {
      "type":   "function_call_output",
      "call_id":"call_abc123",
      "output": "{\"result\":\"success\"}"
    }
  ]
}
```

---

## Sequence — happy path

```
Device                          Gateway
  │───[WS upgrade]────────────────▶│
  │◀──[101 Switching Protocols]────│
  │                                │
  │───[TEXT hello]────────────────▶│  HELLO + HELLO_ACK + EVENT(connected) + STATUS(listening)
  │◀──[TEXT hello_ack]────────────│
  │◀──[TEXT event connected]──────│
  │◀──[TEXT status listening]─────│
  │                                │
  │───[BIN 0x10 Frame × 50]──────▶│  continuous audio
  │◀──[TEXT status thinking]──────│  ASR detected speech
  │◀──[TEXT function_call]────────│  LLM asks device to set emotion=happy
  │◀──[TEXT function_call_output]│  *device replies (required)*
  │───[TEXT function_call_output]▶│  ← from device
  │◀──[TEXT text "好的…"]────────│  LLM produces reply text
  │◀──[BIN 0x11 Start]───────────│  TTS stream
  │◀──[BIN 0x10 Frame × N]───────│
  │◀──[BIN 0x12 End]─────────────│
  │◀──[TEXT status answering]────│
  │◀──[TEXT status answer_finished]│
  │◀──[TEXT status listening]────│  ready for next turn
  │                                │
  │───[WS CLOSE]─────────────────▶│
```

---

## Sequence — barge-in

When the user speaks over the AI (mid-answer), device sends:

```
Device                       Gateway
  │───[BIN 0x13 Cancel]──────────▶│   drop pending TTS, flush buf
  │◀──[TEXT status interrupted]──│
  │◀──[TEXT status listening]────│
  │───[BIN 0x11 Start]───────────▶│   new turn begins
```

---

## Close codes

| Code | When |
|------|------|
| 1000 | Normal closure |
| 1001 | Going away (server shutdown) |
| 4401 | Authentication failed (`hello_err` followed by close) |
| 4404 | Session not found |
| 4429 | Rate-limited |
| 1011 | Internal error |

---

## Compatibility with `ai-hardware-agent-examples` device SDK

The device-side `sdk_integration/convai_bridge.c` already calls the same
lifecycle methods. Mapping to this protocol:

| Device call (convai_bridge.c)            | Wire message |
|------------------------------------------|--------------|
| `convai_create` with `info` + `ws.audio.codec` | `hello` body |
| `convai_start` → server emits `EVENT_CONNECTED` | `event connected` |
| `convai_send_audio(g711a)`              | `BINARY AudioFrame` |
| `on_convai_message_data` (function call)| `function_call` |
| `on_convai_conversation_status`         | `status` |
| `on_convai_event`                       | `event` |
| `convai_update(json)`                   | `config_update` |
| `convai_send_message(json)`             | `function_call_output` |

To wire up a real device to this gateway, the existing `convai_bridge.c`
should "just work" once the device's WS endpoint is repointed at
`ws://<host>:9000/`.

> **Note:** The shipped SDK uses mbedtls over TCP for its WebSocket.
> Because this gateway speaks plaintext WS, point the device at it only
> over a trusted LAN or via a TLS-terminating proxy. See
> [Deployment → TLS](deployment.md#tls).

---

## Versioning

The `MsgType` strings encode the protocol version implicitly (the
`type` field is a string, not an integer). Future extensions add new
strings; old gateways ignore unknown strings.

When a breaking change is needed the sub-protocol identifier will
become `convai.v2`. Devices that request `convai.v2` get the new
behaviour; `convai.v1` continues to be served as documented here.