# 云端微服务架构设计（v2.0）

> 输入：`cloud_business_design.md`（单体设计）  
> 目标：将云端拆为 **3 个职责清晰的微服务**，各服务可独立扩展、独立部署

---

## 1. 为什么拆微服务

| 单体痛点 | 微服务收益 |
|---|---|
| 网关（CPU 密集 ws 收发）和 LLM 编排（长耗时 IO）同进程，互相影响 | 各自按负载特征独立扩容 |
| ASR/TTS 突发流量大，把网关 CPU 抢光 | AudioProcess 单独扩容，对网关零影响 |
| 想升级 LLM 框架要重启整服务 | AITalk 独立发布 |
| 一个 bug（比如 LLM SDK 崩溃）拖垮所有设备 | 进程隔离，最小爆炸半径 |
| 团队 3 个方向（协议 / AI 算法 / 业务编排）互相踩 | 服务边界 = 团队边界 |

---

## 2. 服务全景

```
                          设备 (WS63)
                              │
                              │  WSS convai.v1
                              ▼
                ┌─────────────────────────────┐
                │  Service 1: GATEWAY          │
                │  (端口 9443, 公网入口)        │
                │  - WS 协议终结                │
                │  - 设备鉴权                  │
                │  - 会话路由                  │
                │  - 限流 / 配额                │
                │  - 二进制音频零拷贝转发        │
                └────────────┬────────────────┘
                             │
                             │  gRPC + Protobuf
                             │  (内网)
                             ▼
        ┌────────────────────────────────────────┐
        │  Service 2: AUDIO PROCESS              │
        │  (CPU 密集型, 无状态)                   │
        │  - ASR (Whisper / iFlytek)              │
        │  - TTS (Volcano / Edge)                 │
        │  - VAD 检测                             │
        │  - 音频格式转换 (G.711A ↔ PCM16/Opus)   │
        └─────────────┬──────────────────────────┘
                      │ gRPC streaming
                      │ 双向流式 RPC
                      ▼
        ┌────────────────────────────────────────┐
        │  Service 3: AITALK                     │
        │  (业务编排 + 状态管理)                  │
        │  - LLM 推理 (GPT-4o / Qwen / 文心)        │
        │  - Function Calling 路由                  │
        │  - 会话状态机                            │
        │  - 配置管理（5 维）                      │
        │  - 对话历史                              │
        │  - 持久化到 PostgreSQL / Redis           │
        └────────────────────────────────────────┘
                      │
                      ▼
            ┌──────────────────┐
            │ PostgreSQL       │ ← 设备/配置/会话
            │ Redis            │ ← 会话状态、限流
            │ Object Storage   │ ← 音频日志（可选）
            └──────────────────┘
```

---

## 3. 服务 1: GATEWAY（`cloud_gateway/`）

### 3.1 职责
- WebSocket 协议终结（RFC 6455 + sub-protocol `convai.v1`）
- 设备鉴权（product_id/key/secret 校验 + JWT 颁发）
- 会话建立、心跳、断线检测
- 二进制音频 G.711A 流的**零拷贝转发**（网关不解码）
- 配额限流（每设备每秒 / 每天配额）
- 监控指标上报（Prometheus）

### 3.2 不做
- ❌ ASR / TTS / LLM 调用（全部转给下游）
- ❌ 会话状态机（由 AITalk 维护）
- ❌ 配置存储（由 AITalk + DB 维护）

### 3.3 入口
```
wss://api.example.com:9443/v1/ws
```

### 3.4 内部接口

#### 3.4.1 鉴权（gRPC → AITalk）

```protobuf
service AuthService {
    rpc Authenticate(AuthRequest) returns (AuthResponse);
}

message AuthRequest {
    string product_id     = 1;
    string product_key    = 2;
    string product_secret = 3;
    string device_name    = 4;
    string client_version = 5;  // SDK 版本号
}

message AuthResponse {
    bool   ok = 1;
    string session_token = 2;  // JWT
    int64  expires_at = 3;
    string device_id = 4;
    string error_code = 5;      // "AUTH_FAILED" / "RATE_LIMITED" / ...
    string error_message = 6;
}
```

#### 3.4.2 业务消息（gRPC stream → AITalk）

```protobuf
service TalkService {
    rpc Connect(stream ClientMessage) returns (stream ServerMessage);
}

message ClientMessage {
    oneof body {
        Hello         hello           = 1;
        ConfigUpdate  config_update   = 2;
        AudioFrame    audio_frame     = 3;
        AudioEnd      audio_end       = 4;
        FunctionCallOutput function_call_output = 5;
        Bye           bye             = 6;
        Ping          ping            = 7;
    }
    uint32 seq = 100;
    uint64 ts_ms = 101;
}

message ServerMessage {
    oneof body {
        HelloAck       hello_ack       = 1;
        HelloErr       hello_err       = 2;
        Status         status          = 3;
        Event          event           = 4;
        Text           text            = 5;
        FunctionCall   function_call   = 6;
        ConfigUpdateAck config_update_ack = 7;
        ConfigUpdateErr config_update_err = 8;
        Pong           pong            = 9;
        AudioFromDevice audio_from_device = 10;   // 网关 → AITalk 转发音频
    }
    uint32 seq = 100;
    uint64 ts_ms = 101;
}
```

**关键**：网关把设备 → AITalk 的 `AudioFrame` 原样转发（一字节都不解码），同时把 AITalk → 设备的 `Text / FunctionCall / Status` 等文本消息透传。

#### 3.4.3 AITalk 反馈给设备的消息流（stream）

AITalk 通过 gRPC stream 反向发消息，网关按 WS 协议序列化后推送设备。

### 3.5 配置变更的热更新

`config_update` 直接由 AITalk 处理（DB 校验 + 翻译），网关只是路由。

### 3.6 水平扩展

| 场景 | 策略 |
|---|---|
| 1 万设备并发 | 网关部署 3-5 个实例，前置 nginx / envoy 做 4 层 LB |
| 单实例 5000 连接 | 每个实例 epoll 单线程 + 16 worker 线程 |
| 网关崩溃 | device 重连到其他实例（无状态） |

### 3.7 资源占用预估

| 实例 | CPU | 内存 | 网络 |
|---|---|---|---|
| 小型（1000 设备） | 4 核 | 4 GB | 100 Mbps |
| 中型（5000 设备） | 16 核 | 16 GB | 1 Gbps |

---

## 4. 服务 2: AUDIO PROCESS

### 4.1 职责
- 接收音频流（gRPC bidirectional stream）
- **VAD 检测**：判断说话起止，决定何时关闭一个 turn
- **ASR**：把音频转成文字
- **TTS**：把文字合成音频（G.711A 或 Opus）
- 音频格式转换（G.711A ↔ PCM16 / Opus）

### 4.2 不做
- ❌ LLM 调用
- ❌ Function Calling 路由
- ❌ 会话状态
- ❌ 设备鉴权

### 4.3 接口（gRPC）

```protobuf
service AudioProcessService {
    // 流式：上游传 20ms G.711A 帧，ASR 返回 text
    rpc ProcessAudio(stream AudioChunk) returns (stream AudioEvent);
}

message AudioChunk {
    bytes  g711a_data    = 1;   // 20ms @ 8kHz mono = 160 bytes
    uint64 timestamp_ms  = 2;
    string session_id    = 3;
    string device_id     = 4;
}

message AudioEvent {
    oneof body {
        AsrResult       asr_result       = 1;
        VadEvent        vad_event        = 2;   // SPEECH_START / SPEECH_END
        TtsRequest      tts_request      = 3;   // AITalk → AudioProcess 反向调用
        Error           error            = 4;
    }
}

message AsrResult {
    string text         = 1;
    bool   is_final     = 2;
    float  confidence   = 3;
    int64  latency_ms   = 4;
}

message VadEvent {
    enum Type { SPEECH_START = 0; SPEECH_END = 1; }
    Type type = 1;
    int64 timestamp_ms = 2;
}
```

### 4.4 反向调用（TTS）

AITalk 调 AudioProcess 的 TTS（也是 streaming RPC）：

```protobuf
rpc Synthesize(TtsRequest) returns (stream TtsChunk);

message TtsRequest {
    string text          = 1;
    string voice_type    = 2;
    string session_id    = 3;
    int32  sample_rate   = 4;   // 8000 / 16000 / 24000
    string codec         = 5;   // "g711a" / "opus" / "pcm16"
}

message TtsChunk {
    bytes  audio_data   = 1;
    bool   is_last      = 2;
}
```

### 4.5 内部模块（独立进程内的子模块，可独立扩缩）

| 模块 | 推荐实现 | 部署单元 |
|---|---|---|
| ASR | Whisper-large-v3 / iFlytek 流式 / Paraformer | GPU 节点 × N |
| VAD | Silero VAD / FSMN | 轻量 CPU 节点 |
| TTS | Volcano / Edge / CosyVoice | GPU 节点 × N（高负载） |
| 音频格式 | sox / ffmpeg / 自写 | CPU |

### 4.6 横向扩展

- 流量特征：**CPU 密集 + 大带宽**
- 推荐 2-4 倍 AudioProcess 实例，按 `(QPS, GPU 利用率)` 弹性扩缩
- K8s deployment 上配 HPA（Horizontal Pod Autoscaler）

### 4.7 资源占用预估

| 实例 | CPU | GPU | 内存 | 网络 |
|---|---|---|---|---|
| Whisper-large-v3 | 8 核 | 1× A10 | 16 GB | 200 Mbps |
| Volcano TTS | 4 核 | 1× T4 | 8 GB | 100 Mbps |

---

## 5. 服务 3: AITALK

### 5.1 职责（业务编排 + 状态管理）
- 接收 ASR 文本 → 调 LLM → 流式返回文字 + function calls
- Function Calling 路由（已知工具 → 透传给设备的 WebSocket）
- 会话状态机（IDLE / LISTENING / THINKING / ANSWERING / INTERRUPTED / ANSWER_FINISHED）
- 配置中心（5 维：avatar / voice / personality / relationship / api_key）
- 会话历史、对话日志
- 持久化（PostgreSQL + Redis）

### 5.2 不做
- ❌ 音频处理（ASR/TTS 全交给 AudioProcess）
- ❌ WebSocket 协议（交给 Gateway）
- ❌ 设备鉴权（交给 Gateway）

### 5.3 接口（gRPC，与 Gateway 对接）

```protobuf
service TalkOrchestrator {
    rpc Connect(stream ClientFrame) returns (stream ServerFrame);
}

message ClientFrame {
    oneof body {
        Hello          hello          = 1;
        ConfigUpdate   config_update  = 2;
        FunctionCallOutput function_call_output = 3;
        AudioBoundary  audio_boundary = 4;   // VAD start/end (from AudioProcess)
        AsrFinal       asr_final      = 5;
        Bye            bye            = 6;
    }
    string session_id = 100;
}

message ServerFrame {
    oneof body {
        Status           status            = 1;
        Event            event             = 2;
        Text             text              = 3;
        FunctionCall     function_call     = 4;
        ConfigUpdateAck  config_update_ack = 5;
        ConfigUpdateErr  config_update_err = 6;
        AskAsr           ask_asr           = 7;     // 请求 ASR (内部)
        AskTts           ask_tts           = 8;     // 请求 TTS (内部)
    }
    string session_id = 100;
}
```

### 5.4 内部模块（同一进程内，但有清晰边界）

| 模块 | 职责 |
|---|---|
| SessionStore | 维护 session_id → Session 的内存表 |
| Session | 单设备的对话状态机、上下文、配置缓存 |
| LlmGateway | 调用 LLM（OpenAI / Qwen / 文心 等），流式返回 |
| ToolRouter | 解析 function_call，决定调用本机工具 or 透传给设备 |
| ConfigService | 5 维配置读 / 写 / 翻译（device ↔ 数据库） |
| HistoryService | 会话历史、摘要（短期存 Redis，长期写 PG） |
| Metering | token 计数、配额检查、计费 |

### 5.5 Function Calling 路由表

| tool name | 调用方 | 落地 |
|---|---|---|
| `emotion` | 设备 | → 透传给设备 WS（Gateway 转） |
| `play_sound` | 设备 | → 透传给设备 WS |
| `set_volume` | 设备 | → 透传给设备 WS |
| `web_search` | 云端 | AITalk 内 ToolRouter 调外部搜索 API |
| `knowledge_base_query` | 云端 | AITalk 调知识库 |
| `calendar_event` | 云端 | AITalk 调用户日历 |

### 5.6 配置变更流（关键路径）

```
[设备] ─[config_update]─▶ [Gateway] ─[透传]─▶ [AITalk.ConfigService]
                                            │
                                            ├─ 校验字段
                                            ├─ 翻译: voice_type_id → provider voice_id
                                            ├─ 拼装完整 system_messages
                                            ├─ 写 PostgreSQL
                                            ├─ 更新 Session 配置缓存
                                            ├─ [可选] 通知 LlmGateway 切换 system prompt
                                            └─ 回 ack / err
```

### 5.7 横向扩展

- AITalk 是**有状态**服务（每个 session_id 对应一台实例）—— 需要 sticky session
- 状态外置到 Redis（会话历史、配置缓存）
- 实例无状态化后，可以按 session_id 取模路由

### 5.8 资源占用预估

| 实例 | CPU | 内存 | 备注 |
|---|---|---|---|
| 1000 并发 session | 8 核 | 16 GB | 每 session 几 MB 历史 |
| LLM 调用密集 | 16 核 | 32 GB | 流式 token 缓冲 |

---

## 6. 端到端时序（一个完整回合）

```
设备        Gateway          AITalk           AudioProcess
 │             │                │                  │
 │─WS Hello──▶│                │                  │
 │             │─gRPC Auth─────▶│                  │
 │             │◀─session_tok──│                  │
 │◀─hello_ack─│                │                  │
 │◀─event con─│                │                  │
 │◀─status────│                │                  │
 │             │                │                  │
 │─Bin Audio──▶│                │                  │
 │─Bin Audio──▶│─gRPC Audio────▶│                  │
 │             │                │─ProcessAudio────▶│
 │             │                │                  │─[Whisper]
 │             │                │◀─AsrResult──────│
 │─Bin End────▶│─gRPC AudioEnd─▶│                  │
 │             │                │                  │
 │             │                │─[LLM chat]──────▶│GPT-4o
 │             │                │                  │
 │             │                │◀─[stream chunks]│
 │             │◀─status ans────│                  │
 │             │◀─text "好呀！"─│                  │
 │             │◀─func_call────│                  │
 │             │─gRPC func_ack─▶│  (设备应答)       │
 │             │                │                  │
 │             │                │─Synthesize──────▶│Volcano
 │             │                │◀─TtsChunk────────│
 │             │◀─Bin Start─────│                  │
 │             │◀─Bin Frame─────│                  │
 │             │◀─Bin End───────│                  │
 │             │◀─status finish─│                  │
 │             │◀─status listen─│                  │
```

---

## 7. 服务间通信模式

| 调用方向 | 协议 | 备注 |
|---|---|---|
| 设备 → Gateway | WSS convai.v1 | 公网入口 |
| Gateway → AITalk | gRPC bidirectional stream | 内网 |
| AITalk → AudioProcess | gRPC streaming RPC | 内网 |
| AITalk → PostgreSQL | pgx / psycopg | 内网 |
| AITalk → Redis | redis-py / go-redis | 内网 |
| AudioProcess → 外部 ASR | HTTPS / gRPC | 外网 / 内网 |

**为什么 gRPC 而不是消息队列？**
- 实时对话延迟敏感（< 1s end-to-end）
- 一问一答的会话本身就是 RPC 模式
- 消息队列（Kafka）适合离线分析、批量处理

---

## 8. 部署架构

### 8.1 K8s 部署（推荐）

```yaml
# gateway-deployment.yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: cloud-gateway
spec:
  replicas: 5
  selector:
    matchLabels: { app: cloud-gateway }
  template:
    spec:
      containers:
        - name: gateway
          image: cloud-gateway:0.1.0
          ports: [{ containerPort: 9000 }]
          resources:
            requests: { cpu: "1",  memory: "1Gi" }
            limits:   { cpu: "4",  memory: "4Gi" }
          readinessProbe:
            exec: { command: ["/cloud_gateway", "--probe"] }
---
# audio-process-deployment.yaml (GPU node)
spec:
  replicas: 4
  nodeSelector: { workload: gpu }
  containers:
    - name: audio
      image: audio-process:0.1.0
      resources:
        requests: { cpu: "4", memory: "8Gi", "nvidia.com/gpu": 1 }
---
# ai-talk-deployment.yaml
spec:
  replicas: 8
  containers:
    - name: talk
      image: ai-talk:0.1.0
      resources:
        requests: { cpu: "2", memory: "4Gi" }
```

### 8.2 前置 L4/L7 LB

```
device ─DNS─▶ envoy (TLS终结, WS upgrade)
              ├─▶ cloud-gateway:9000 (round-robin)
              ├─▶ cloud-gateway:9000
              └─▶ cloud-gateway:9000
```

Gateway 之间**无状态**，envoy 直接 4 层负载均衡。

AITalk 需要 sticky session（基于 session_id 哈希），envoy 用 ring hash。

---

## 9. 数据持久化

| 数据 | 存储 | 保留期 | 用途 |
|---|---|---|---|
| 设备信息 | PostgreSQL `devices` | 永久 | 鉴权、统计 |
| 5 维配置 | PostgreSQL `device_configs` | 永久 | 恢复会话 |
| 会话历史（短期）| Redis `session:{id}:history` | 24h | LLM 上下文 |
| 会话历史（长期）| PostgreSQL `sessions` | 90 天 | 对话分析 |
| 限流计数器 | Redis | 1 分钟滑动窗口 | 防滥用 |
| 音频日志 | Object Storage (S3) | 可选 | 质量回溯 |
| 监控指标 | Prometheus + Thanos | 13 个月 | 长期趋势 |

---

## 10. 关键设计决策

| 决策 | 选 A | 选 B | 选 A 的理由 |
|---|---|---|---|
| 音频传输 | **原始 G.711A 流式转发** | 网关解码后再编码 | A、B 都是云端原始字节，解码是无用功 |
| 状态管理 | **AITalk 持有 Session** | Redis 集中管理 | 状态变更频繁，Redis IO 开销大 |
| Function call 透传 | **网关直接转发** | AITalk 解析后重组 | 减少一次序列化，且 tool schema 演进不需网关改 |
| 配置拼装 | **AITalk 重新拼** | 直接信任设备 | 防设备被破解、版本升级的灵活性 |
| 网关无状态 | **是** | 否 | 水平扩展、灾备简单 |

---

## 11. 与现有 cloud_gateway 代码的迁移路径

`cloud_gateway/`（C++）目前是单体。新架构需要**逐步拆分**：

| 阶段 | 改动 | 工作量 |
|---|---|---|
| Phase 1 | cloud_gateway 现状 + 真实后端（替换 stub） | 1-2 周 |
| Phase 2 | 把 ASR/TTS 从 gateway 抽出到 AudioProcess 进程，gateway 走 gRPC | 2-3 周 |
| Phase 3 | 抽出 AITalk 业务（LLM / 配置 / Function router），gateway 只做协议 | 2-3 周 |
| Phase 4 | 引入 K8s + 服务网格（envoy / istio） | 1-2 周 |

---

## 12. 失败回滚与降级

| 故障 | 降级策略 |
|---|---|
| AudioProcess 不可用 | AITalk 返回 text-only 模式，UI 显示"语音暂不可用" |
| AITalk 不可用 | Gateway 返回 503，设备进入降级 UI |
| PostgreSQL 不可用 | AITalk 用 Redis 缓存配置（最后已知值），写操作排队 |
| Redis 不可用 | AITalk 回退到内存缓存，重新启动会丢失短期上下文 |
| LLM 服务商不可用 | AITalk 切换备用 LLM（如 GPT-4o → Claude） |
| 整机房宕机 | DNS 切到备用 region |

---

## 13. 服务边界 = 团队边界

| 团队 | 服务 | 主要语言 | 核心技能 |
|---|---|---|---|
| 平台组 | Gateway | C++ / Go | 网络协议、高性能 IO |
| AI 平台组 | AudioProcess | Python | ASR/TTS、GPU 优化 |
| 业务组 | AITalk | Go / Python | LLM 编排、业务逻辑 |
| SRE | K8s / 监控 / 告警 | — | — |

每个团队独立迭代、发布、回滚。

---

## 14. 总结

```
Gateway      = 设备协议层 (WS, 鉴权, 路由)
AudioProcess = 音频处理层 (ASR, TTS, VAD)
AITalk       = 业务编排层 (LLM, 配置, FC, 状态)
```

- Gateway：薄、稳、高性能
- AudioProcess：CPU/GPU 密集、无状态、弹性扩缩
- AITalk：业务编排、有状态（Redis 外置）、集成 LLM

三层解耦后，每层可以独立选型、独立扩容、独立故障隔离。设备端 settings app 完全无感——它只跟 Gateway 说话，所有复杂性都在云端内部。