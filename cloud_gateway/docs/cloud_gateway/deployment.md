# Deployment

This document covers building, running, and hardening the gateway for
real-world use.

---

## Build

### Requirements

| Tool      | Version | Notes |
|-----------|---------|-------|
| CMake     | ≥ 3.16  | Tested with 3.22+ |
| g++/clang | C++17   | Tested with g++ 11 / clang 14 |
| Linux     | 5.x+    | Required (epoll, `SOCK_NONBLOCK`, `accept4`) |
| macOS     | 12+     | Works (uses kqueue is a future extension; today relies on epoll via `ENABLE_EPOLL=1`) |

> Windows is **not supported** in this build. The device-side SDK
> already runs Windows for development, but the gateway needs Linux
> sockets / `epoll`. If you need Windows, port `src/ws/server.cpp`
> to IOCP.

### Commands

```bash
cd cloud_gateway

# Release build (recommended)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Debug build with verbose logging
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DGATEWAY_WARN_AS_ERR=OFF
cmake --build build -j$(nproc)

# Tests off (smaller binary)
cmake -B build -DGATEWAY_BUILD_TESTS=OFF
cmake --build build
```

Artifacts:

| File | Purpose |
|------|---------|
| `build/cloud_gateway` | the server binary |
| `build/gateway_test_client` | the C++ test client |

### Install

```bash
cmake --install build --prefix /usr/local
# installs to /usr/local/bin/cloud_gateway and gateway_test_client
```

---

## Run

### Local sanity check

```bash
# Terminal A
./build/cloud_gateway --port 9000 --log-level debug

# Terminal B
./build/gateway_test_client --host 127.0.0.1 --port 9000 --audio-frames 50
```

You should see:

```
[client] handshake ok, subproto ok
[client] hello sent
[client] sent 50 audio frames
[recv] TEXT   len=128  {"type":"hello_ack", ...}
[recv] TEXT   len=68   {"type":"event","event":"connected", ...}
[recv] TEXT   len=65   {"type":"status","status":"listening"}
[recv] TEXT   len=240  {"type":"function_call", ...}
[recv] TEXT   len=34   {"type":"text","text":"好的，听到你说："}
[recv] BINARY len=... audio op=0x11 seq=0     (TTS Start)
[recv] BINARY len=... audio op=0x10 seq=1     (TTS Frame)
... (more frames)
[recv] BINARY len=13  audio op=0x12 seq=0     (TTS End)
```

### Server flags

```
--bind ADDR          default 0.0.0.0
--port N             default 9000
--workers N          default 4
--max-payload N      default 65536
--log-level LEVEL    debug|info|warn|error
--asr NAME           default stub
--llm NAME           default stub
--tts NAME           default stub
```

### Run as a systemd service

`/etc/systemd/system/cloud-gateway.service`:

```ini
[Unit]
Description=ConvAI Cloud Gateway
After=network.target

[Service]
Type=simple
User=gateway
Group=gateway
WorkingDirectory=/opt/cloud_gateway
ExecStart=/usr/local/bin/cloud_gateway --bind 0.0.0.0 --port 9000 --workers 8
Restart=on-failure
RestartSec=2s
LimitNOFILE=65536
Environment=GATEWAY_LOG_LEVEL=info

[Install]
WantedBy=multi-user.target
```

Enable + start:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now cloud-gateway
sudo journalctl -u cloud-gateway -f
```

---

## Hardening

The reference build is **plaintext** and has **no built-in auth**. For
production, take the following steps.

### TLS

Do NOT terminate TLS inside the gateway (yet). Put nginx in front:

```nginx
stream {
    upstream gateway {
        server 127.0.0.1:9000;
    }

    server {
        listen 9443 ssl;
        proxy_pass gateway;
        proxy_protocol off;

        ssl_certificate     /etc/ssl/certs/gateway.crt;
        ssl_certificate_key /etc/ssl/private/gateway.key;
        ssl_protocols TLSv1.2 TLSv1.3;
        ssl_ciphers HIGH:!aNULL:!MD5;
    }
}
```

Point the device at `wss://gateway.example.com:9443/`.

For a richer setup, use HAProxy with stickiness on the device name
(`Sec-WebSocket-Key` is unique per connection, so simple round-robin
works).

### Authentication

Today the gateway accepts any device that performs the WS handshake.
A production deployment should add at least:

1. Validate `product_id / product_key / product_secret` in `hello`
   body against a database (Postgres / Redis cache).
2. Issue a short-lived `session_token` and require it on subsequent
   `config_update` messages.
3. Reject connections with too-frequent `hello` attempts
   (rate-limiting at the gateway or via nginx limit_req).

### Rate limiting

The gateway itself doesn't rate-limit (the io thread is hot path).
Limit at the LB or implement a per-session token bucket in
`Session::on_audio_binary` before submitting to ASR.

### Metrics

The gateway does not yet export metrics. Quick wins:

- `n_active_sessions = session_manager.count()` every 10 s
- per-`asr/llm/tts` call latency (worker thread is a good hook)
- WS-level `recv`/`send` byte counters per session

Expose via Prometheus (`/metrics` HTTP endpoint on a different port)
or push to statsd from a sidecar thread.

### Logging

Logs go to stderr in human format. To ship to a central log store:

```bash
./cloud_gateway 2>&1 | tee -a /var/log/cloud_gateway.log
```

Or use `journalctl` if you run under systemd.

Levels are controlled by `--log-level` / `GATEWAY_LOG_LEVEL`. For
production `info` is recommended; use `debug` only when troubleshooting
specific sessions (it logs every audio frame's energy).

---

## Troubleshooting

### "handshake timeout" on the test client

The server isn't reachable. Check:

```bash
ss -tlnp | grep 9000
curl -v http://127.0.0.1:9000/
# Expected: 426 Upgrade Required (HTTP server rejecting WS without upgrade header)
```

### "frame parse error"

Usually a protocol mismatch (different `Sec-WebSocket-Protocol` or a
device sending fragmented frames). Enable `--log-level debug` and look
for the exact offset that failed.

### High CPU

Either:
- `StubLlm` is being hit on every audio frame (device is sending
  `AUDIO_FRAME` 0x10 too aggressively — check sample rate).
- Nagle's algorithm is enabled on the gateway side. The build uses
  `TCP_NODELAY` already (see `WsServer::on_accept`).

### Sessions leak

If `SessionManager::count()` keeps growing while the device count is
steady, a peer hung up without sending CLOSE and we missed it. The
heartbeat timer (`tick_heartbeat`) should close it after 60 s; check
that the io thread isn't starved by long-running worker callbacks.

---

## Performance notes

On a single core of a modest Linux VM the reference gateway should
sustain **~500 concurrent idle sessions** and **~50 concurrent active
sessions** (ASR + LLM + TTS stub takes ~5 ms per request). Real ASR /
LLM / TTS will dominate wall-clock latency and you'll want more
workers.

For higher throughput:

1. Increase `--workers N` (one per CPU core is a good start)
2. Pin the io thread to core 0, workers to cores 1..N
3. Replace stubs with batched real backends (e.g. Whisper-large-v3
   processes 30 s of audio in <1 s on a GPU)

---

## Source layout reminder

```
cloud_gateway/
├── CMakeLists.txt
├── README.md / README_zh.md
├── docs/cloud_gateway/
├── include/cloud_gateway/   # public headers
├── src/
│   ├── main.cpp
│   ├── config.cpp
│   ├── protocol.cpp
│   ├── util/                 # json, base64, sha1
│   ├── ws/                   # RFC 6455 + epoll server
│   ├── codec/                # envelope <-> JSON, audio <-> bytes
│   ├── server/               # thread pool
│   ├── session/              # per-connection session logic
│   └── upstream/             # ASR / LLM / TTS interfaces + stubs
└── test/test_client.cpp
```

See [API reference](api_reference.md) for per-class documentation.