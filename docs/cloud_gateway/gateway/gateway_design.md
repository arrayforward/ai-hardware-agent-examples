# Gateway 服务设计

> 服务代号：`cloud-gateway`  
> 实现位置：`D:\ai-hardware-agent-examples\cloud_gateway\`（C++17，已完成 PoC）  
> **下游：MediaService（不再直接连 AITalk）**

---

## 1. 单一职责

> **Gateway 只做一件事：把设备的 WebSocket 字节流翻译成对 MediaService 的 gRPC 流。**

具体：
- WebSocket 协议终结（RFC 6455 + `convai.v1`）
- 设备鉴权（调 AITalk.AuthService 拿 JWT，**经 MediaService 中转**）
- 会话路由（gRPC stream 到 MediaService）
- **音频字节零拷贝转发**（不解码、不分析）
- 配额限流（每设备 / 每秒 / 每天）
- 监控指标（Prometheus）

## 2. 不做的事

- ❌ ASR / TTS / VAD / 降噪（交给 MediaService）
- ❌ 会话状态机（由 AITalk 持有）
- ❌ 配置存储（由 AITalk + DB 维护）
- ❌ 业务错误码翻译（仅透传 AITalk 返回的 code/message）

---

## 3. 位置（重新定义）

```
设备 ←→ Gateway ←→ MediaService ←→ AITalk
       (C++)        (Python+GPU)     (Go/Python)
       协议终结       音频 AI          业务编排
```

Gateway 与 AITalk 之间**没有直接连接**。所有业务流都经 MediaService 中转。

---

## 4. 架构

```
   设备  ─WSS convai.v1─▶  Gateway  ─gRPC stream─▶  MediaService
                              │                          │
                              ├─ 鉴权 RPC ───────────────▶│ (转发到 AITalk)
                              │                          │
                              ├─ 业务消息透传 ──────────▶│
                              │                          │
                              ├─ 音频流二进制转发 ──────▶│
                              │                          │
                              │◀─ AITalk 转发的文字 + FC ──│
                              │◀- TTS 合成的音频 ──────────│
                              │
                              ├─ 配额 Redis (token bucket)
                              ├─ 监控 Prometheus
                              └─ 写日志 stderr → Loki/ELK
```

---

## 5. 协议终结

### 5.1 入站（设备 → Gateway）

| 类型 | 处理 |
|---|---|
| WS upgrade | 调 MediaService.AuthService（转 AITalk），通过则握手 101 |
| TEXT hello | 透传 → MediaService → AITalk hello 帧 |
| TEXT config_update | 透传 → MediaService → AITalk config_update 帧 |
| TEXT function_call_output | 透传 → MediaService → AITalk function_call_output 帧 |
| TEXT bye | 关闭 stream、通知 MediaService 清理 |
| BIN AudioFrame (op=0x10) | 透传 → MediaService → AITalk audio_frame |
| BIN AudioEnd (op=0x12) | 透传 → MediaService → AITalk audio_boundary |

### 5.2 出站（MediaService → Gateway → 设备）

| MediaService 上行消息 | 设备侧消息 |
|---|---|
| `hello_ack` | TEXT hello_ack |
| `hello_err` | TEXT hello_err（随后 WS close 4xxx） |
| `status` | TEXT status |
| `event` | TEXT event |
| `text` | TEXT text |
| `function_call` | TEXT function_call |
| `config_update_ack` | TEXT config_update_ack |
| `config_update_err` | TEXT config_update_err |
| `audio_chunk` (TTS) | BIN AudioFrame（Start/Frame/End） |
| `pong` | TEXT pong |

**关键**：Gateway 只做协议转换，**不知道音频如何处理**——所有 VAD/ASR/TTS 都在 MediaService 内部。

---

## 6. 鉴权流程（经 MediaService 中转）

```
设备 → Gateway (hello body 含 product_id/key/secret/device_name)
      │ Gateway 不持有设备数据库，直接转给 MediaService
      ▼
MediaService → AITalk.AuthService.Authenticate
      │ AITalk 校验凭证（DB + bcrypt）
      ▼
返回 JWT (24h expiry)
      │
      ▼ Gateway 缓存 JWT，后续 gRPC stream 用此 JWT
```

**注意**：Gateway 与 MediaService 之间的 gRPC stream 也带 JWT 鉴权（Header 携带），
由 MediaService 校验 JWT 中的 `aud` 字段。

---

## 7. 限流

### 7.1 三层配额（每设备维度）

| 维度 | 限制 | 超出行为 |
|---|---|---|
| 连接频率 | 1 次 / 10s | 返回 `RATE_LIMITED` |
| 并发连接 | 1 / device | 第二次 hello 返 `ALREADY_CONNECTED` |
| 上行带宽 | 32 KB/s（滑动窗口 1s） | 多余字节丢弃 |
| 单 session 时长 | 60 min | WS close 4429 |
| 每日音频时长 | 60 min | `RATE_LIMITED` |

### 7.2 实现
- Redis token bucket：key = `ratelimit:{device_id}:{dimension}`
- 每条流式消息进入时 `INCR + EXPIRE`
- 超限直接 close 并返回错误码

---

## 8. 会话生命周期

```
device open TCP
   ↓
accept → fd
   ↓
WS handshake
   ↓
[optional] hello frame 解析
   ↓
gRPC stream.Connect to MediaService (GatewayBridge)
   ↓
stream.Run (双工转发)
   ↓
device close / heartbeat timeout / MediaService closes stream
   ↓
close fd, drop connection
```

每个 WS 连接对应 1 个 MediaService stream。

---

## 9. 资源占用预估（单实例）

| 维度 | 估算 |
|---|---|
| CPU | 2-4 核（epoll 单线程 + 转发 IO） |
| 内存 | 2-4 GB（每连接 < 4 KB） |
| 网络 | 1 Gbps 出口 / 1 Gbps 入口 |
| 文件描述符 | ulimit -n 100 万以上 |

5000 并发连接下：
- CPU 占用 < 50%
- 内存 < 2 GB
- 网络 500 Mbps 双向

---

## 10. 水平扩展

- 无状态实例，N 副本即可线性扩展
- 前置 envoy/Nginx 4 层 LB（TCP）

```yaml
# gateway-deployment.yaml (K8s)
apiVersion: apps/v1
kind: Deployment
metadata: { name: cloud-gateway }
spec:
  replicas: 5
  template:
    spec:
      containers:
        - name: gateway
          image: cloud-gateway:0.1.0
          ports: [{ containerPort: 9443 }]
          env:
            - { name: MEDIASERVICE_GRPC_TARGET, value: "media-service-grpc:50051" }
            - { name: REDIS_URL,              value: "redis://redis:6379" }
```

---

## 11. 配置

| 环境变量 | 默认值 | 说明 |
|---|---|---|
| `LISTEN_PORT` | 9443 | WS 监听端口 |
| `LISTEN_ADDRESS` | `0.0.0.0` | 监听地址 |
| `HEARTBEAT_SECONDS` | 60 | 无 ping/pong 则关闭 |
| `MAX_PAYLOAD_BYTES` | 65536 | WS 单帧最大 |
| `MEDIASERVICE_GRPC_TARGET` | `media-service-grpc:50051` | 下游 gRPC |
| `REDIS_URL` | `redis://redis:6379` | 限流 / session 缓存 |
| `LOG_LEVEL` | `info` | debug/info/warn/error |

---

## 12. 监控指标

```
# HELP cloud_gateway_active_connections Current WebSocket connections
# TYPE cloud_gateway_active_connections gauge
cloud_gateway_active_connections{instance="..."} 4321

# HELP cloud_gateway_messages_in_total Inbound WS messages
# TYPE cloud_gateway_messages_in_total counter
cloud_gateway_messages_in_total{type="text",instance="..."} 1234567

# HELP cloud_gateway_messages_out_total Outbound WS messages
# TYPE cloud_gateway_messages_out_total counter
cloud_gateway_messages_out_total{type="audio_frame",instance="..."} 9876543

# HELP cloud_gateway_ratelimit_hits_total Rate limit exceeded
# TYPE cloud_gateway_ratelimit_hits_total counter
cloud_gateway_ratelimit_hits_total{dimension="bandwidth",instance="..."} 23
```

---

## 13. 故障行为

| 场景 | 行为 |
|---|---|
| MediaService gRPC 不可达 | hello_ack 阶段失败 → 返 `hello_err(SERVICE_UNAVAILABLE)` → WS close 1011 |
| MediaService stream 中途断开 | Gateway 把 stream 关闭，设备收 `event disconnected` + WS close |
| Redis 不可用 | 限流退化为"无限流"（fail-open），告警 |
| 自身 OOM | K8s 重启实例，前置 LB 把流量切走 |

---

## 14. 与设备 SDK 的兼容性

- 不修改设备 SDK 代码
- 不修改 `convai.v1` 子协议
- 关闭码 4xxx 完全沿用已有定义（4401 / 4429 等）

---

## 15. 当前实现状态

`cloud_gateway/` 目录下的 C++ PoC 已完成：
- ✅ WS 协议终结（含握手、帧解析、跨平台 epoll/select）
- ✅ 文本/二进制双帧编解码
- ✅ JSON 信封 + G.711A 音频
- ✅ 单元测试 34/36 通过
- ✅ 集成测试跑通 hello → audio → LLM → TTS → emotion FC 全流程
- ✅ Linux + Windows 跨平台编译

**当前实现的下游**：stub in-process（asr/llm/tts 全部本地模拟）。
**下一步**：把 stub 替换为 gRPC 客户端调用 MediaService。

待改进：
- gRPC 客户端（替换 stub）
- Prometheus exporter
- Redis 限流
- TLS 终结（当前明文，由前置 envoy 处理）