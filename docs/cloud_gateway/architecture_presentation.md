# ConvAI Cloud 微服务架构 — 架构设计 PPT

> 本文档汇总了云端架构的所有设计要点，按演示文稿（slide）格式组织。  
> 可直接用作汇报材料，也可通过 pandoc 一键导出为真正的 PPTX。

---

## 📑 Slide 1 — 封面

```
┌─────────────────────────────────────────────────────────────┐
│                                                             │
│         ConvAI Cloud 架构设计                               │
│                                                             │
│         ─── 微服务化拆分与边界设计 ───                       │
│                                                             │
│         版本：v2.0                                          │
│         日期：2026-07-17                                    │
│         关联文档：                                           │
│           - README.md (本目录)                              │
│           - gateway/gateway_design.md                       │
│           - media_service/media_service_design.md           │
│           - ai_talk/ai_talk_design.md                       │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

---

## 📑 Slide 2 — 为什么拆分微服务

| 单体痛点 | 微服务收益 |
|---|---|
| 网关和 LLM 编排同进程，CPU/IO 互相影响 | 各按负载特征独立扩容 |
| ASR/TTS 突发流量抢光网关 CPU | 独立扩缩 |
| LLM SDK 升级要重启所有服务 | 独立发布 |
| 一个 bug 拖垮所有设备 | 故障隔离 |
| 3 个团队互相踩 | 服务边界 = 团队边界 |

---

## 📑 Slide 3 — 调用方向总图

```
设备 (WS63)
   │ WSS convai.v1
   ▼
┌──────────┐    gRPC stream      ┌────────────────┐    gRPC stream      ┌──────────┐
│ Gateway  │ ──────────────────▶ │ MediaService    │ ──────────────────▶ │ AITalk   │
│  (C++)   │   GatewayBridge     │ (Python, GPU)   │    TalkService       │ (Go/Py)  │
└──────────┘   音频字节流        └────────────────┘   只有文字+控制       └──────────┘
                                                                              │
                                                                              ▼
                                                                       ┌──────────────┐
                                                                       │ 外部 LLM     │
                                                                       │ GPT-4o/Qwen  │
                                                                       └──────────────┘
```

**关键**：MediaService 是 Gateway 和 AITalk 之间的"音频 AI 翻译官"。

---

## 📑 Slide 4 — 三层服务定位

| 层级 | 服务 | 职责一句话 | 资源特征 |
|---|---|---|---|
| **入口层** | Gateway | 设备 WS 协议终结 + 鉴权 + 路由 | CPU + 网络 IO |
| **音频 AI 层** | MediaService | 把音频变文字（ASR pipeline）+ 把文字变音频（TTS pipeline） | CPU + GPU 混合 |
| **业务编排层** | AITalk | 拿到文字后决定回复什么（LLM + 配置 + FC + 状态） | CPU |

---

## 📑 Slide 5 — 边界设计原则

| 边界 | 数据形式 | 带宽量级 |
|---|---|---|
| 设备 ↔ Gateway | WS 原始字节流 | 公网 GB/s |
| Gateway ↔ MediaService | gRPC + 音频字节 | 内网万兆 |
| MediaService ↔ AITalk | **gRPC 只有文字** | 内网 KB/s |

**核心原则**：音频字节永远不出 MediaService 边界。

---

## 📑 Slide 6 — Gateway 服务

**职责**：
- WebSocket 协议终结（RFC 6455 + `convai.v1`）
- 设备 hello 鉴权（转 AITalk 验证）
- 业务消息透传到 MediaService（gRPC stream）
- **音频字节零拷贝转发**（不解码）
- 配额限流 + 监控指标

**不做**：
- ❌ ASR / TTS / VAD
- ❌ 会话状态
- ❌ 配置存储

**关键指标**：
- 单实例：5000 并发 WS
- 转发延迟：< 5 ms
- 部署：5-10 实例，CPU 节点

**详细设计**：[gateway/gateway_design.md](gateway/gateway_design.md)

---

## 📑 Slide 7 — Gateway 协议终结流程

```
设备 → Gateway (WS convai.v1)
   │ 解析 HTTP Upgrade
   │ 调 AITalk.AuthService（经 MediaService）→ 拿 JWT
   │ 通过 → 101 Switching Protocols
   ▼
后续每帧：
   TEXT → 透传 → MediaService → AITalk
   BIN AudioFrame → 透传 → MediaService (denoise+vad+asr) → AITalk 文字
   ▼
AITalk 文字 / TTS 文字 → MediaService → 序列化为 WS 帧 → 设备
```

---

## 📑 Slide 8 — MediaService 服务

**职责**：
- **音频前端**：denoise（RNNoise）+ VAD（Silero）
- **ASR pipeline**：Whisper-large-v3 / iFlytek
- **TTS pipeline**：Volcano / CosyVoice
- **格式转换**：G.711A ↔ PCM16 ↔ Opus

**三大子能力**：

| 子能力 | 资源 | 延迟 | 调用频率 |
|---|---|---|---|
| vad_denoise | CPU 轻量 | < 30ms | 高（每 20ms） |
| asr_engine | GPU 重 | 200-1500ms | 中（每回合 1 次） |
| tts_engine | GPU 重 | 300-800ms | 中（每回合 1 次） |

**关键边界**：与 AITalk 之间只传文字，不传音频。

**详细设计**：[media_service/media_service_design.md](media_service/media_service_design.md)

---

## 📑 Slide 9 — MediaService 内部架构

```
MediaService 实例 (Python)
├── GatewayBridge（gRPC 入：从 Gateway 接收设备消息+音频）
├── TalkService    （gRPC 出：向 AITalk 发送文字+控制）
├── PipelineOrchestrator（核心）
│     ├─[上行] AudioFrame → vad_denoise → asr_engine → AsrFinal
│     └─[下行] TtsText → tts_engine → AudioChunk
└── 子模块（默认同进程，可拆分）
      ├── vad_denoise
      ├── asr_engine
      └── tts_engine
```

---

## 📑 Slide 10 — MediaService 上行 ASR Pipeline

```
[T=0ms]    Gateway 发 AudioFrame #1 (G.711A 20ms)
[T=20ms]   AudioFrame #2
...
[T=100ms]  vad_denoise 检测到 speech_start
[T=100ms]  启动 asr_engine 流
[T=200ms]  asr 返回 AsrPartial "今天我们"
[T=300ms]  asr 返回 AsrPartial "今天我们学"
[T=400ms]  asr 返回 AsrFinal "今天我们学画画"
[T=420ms]  vad_denoise 检测到 speech_end
[T=425ms]  关闭 asr_engine 流
[T=430ms]  发 AsrFinal → AITalk
```

**端到端延迟**（VAD end → AsrFinal）：~150-300 ms

---

## 📑 Slide 11 — MediaService 下行 TTS Pipeline

```
[AITalk] TtsText "好呀！"
   │
   ▼
[PipelineOrchestrator] 提交给 tts_engine
   │
   ▼
[tts_engine] 流式合成
   │ AudioChunk(seq=1, 160B G.711A)
   │ AudioChunk(seq=2, 160B G.711A)
   │ ...
   │ AudioChunk(seq=N, 160B G.711A, is_last=true)
   │
   ▼
[GatewayBridge] 序列化 WS 帧
   │
   ▼
[设备] 累积 → 播放器 → 扬声器
```

---

## 📑 Slide 12 — AITalk 服务

**职责**：
- 接收 ASR 文字 → 拼装 LLM prompt → 调 LLM
- 流式返回 text + function calls
- Function Calling 路由
- 5 维配置管理（avatar / voice / personality / relationship / api_key）
- 会话状态机（6 种：IDLE/LISTENING/THINKING/ANSWERING/INTERRUPTED/ANSWER_FINISHED）
- 对话历史 + 配置持久化

**不做**：
- ❌ 设备协议
- ❌ 音频处理
- ❌ 音频字节解析

**资源**：纯 CPU，8-20 实例，按 session_id 哈希路由（session affinity）

**详细设计**：[ai_talk/ai_talk_design.md](ai_talk/ai_talk_design.md)

---

## 📑 Slide 13 — AITalk 状态机

```
                ┌──────────┐
                │ INIT     │ ← 内存创建，未持久化
                └────┬─────┘
                     │ hello_ack 成功
                     ▼
        ┌────────────────────────┐
        │ IDLE (Listening)        │ ← 等待 VadEvent
        └────┬───────────────────┘
             │ VadEvent.SPEECH_START
             ▼
        ┌────────────────────────┐
        │ LISTENING              │ ← ASR 进行中
        └────┬───────────────────┘
             │ AsrFinal
             ▼
        ┌────────────────────────┐
        │ THINKING               │ ← LLM 推理中
        └────┬───────────────────┘
             │ LLM first token
             ▼
        ┌────────────────────────┐
        │ ANSWERING              │ ← 流式 text + 触发 TTS
        └────┬───────────────────┘
             │ 用户打断
             ▼
        ┌────────────────────────┐
        │ INTERRUPTED            │ ← 取消 TTS
        └────┬───────────────────┘
             │
             ▼
        ┌────────────────────────┐
        │ ANSWER_FINISHED         │ ← 短态
        └────┬───────────────────┘
             │ 自动
             ▼
        ┌────────────────────────┐
        │ IDLE                   │
        └────────────────────────┘
```

---

## 📑 Slide 14 — AITalk Function Calling 路由

| 工具名 | 调用方 | 落地 |
|---|---|---|
| `emotion` | 设备 | 经 MediaService 透传给设备 |
| `play_sound` | 设备 | 经 MediaService 透传给设备 |
| `set_volume` | 设备 | 经 MediaService 透传给设备 |
| `navigate_to` | 设备 | 经 MediaService 透传给设备 |
| `show_emoji` | 设备 | 经 MediaService 透传给设备 |
| `web_search` | 云端 | AITalk 内 ToolRouter 调外部搜索 API |
| `knowledge_query` | 云端 | AITalk 调向量库 |
| `calendar_event` | 云端 | AITalk 调用户日历 |
| `reminder_set` | 云端 | AITalk 调 cron |

**关键协议**：完全沿用 OpenAI Realtime API 的 function calling 格式。

---

## 📑 Slide 15 — 配置变更业务流

```
设备发出 config_update（音色/性格/关系）
   ↓
[Gateway] 透传 → MediaService → AITalk.ConfigService.Apply
   ↓
1. 校验字段（voice_type/personality_id/relationship_id 在 catalog 中）
2. 性别一致性（avatar=gender 与 relationship.gender 必须匹配）
3. 防篡改：忽略设备/MediaService 发的 system_messages，从 catalog 取完整 prompt
4. 拼装完整 LLM 配置：
   system_messages = [base_prompt, personality.prompt, relationship.prompt, user_overlay]
   voice_type = voice_catalog.provider_voice
5. 写 PostgreSQL device_configs
6. 更新 Redis session.{uuid}.llm 配置
7. 返回 ack 或 err
```

---

## 📑 Slide 16 — 数据模型

### PostgreSQL（持久化）

```sql
devices              -- 设备主表
device_configs       -- 5 维配置
voice_catalog        -- 音色目录
personality_catalog  -- 性格 prompt
relationship_catalog -- 关系 prompt
sessions             -- 会话日志
```

### Redis（短期状态）

```
session:{uuid}:state      -- 会话状态机
session:{uuid}:history    -- LLM 上下文（24h）
session:{uuid}:llm        -- LLM session ID
device:{id}:qos            -- 限流滑动窗口
```

---

## 📑 Slide 17 — 接口规范：Gateway ↔ MediaService

```protobuf
syntax = "proto3";
package media.v1;

service GatewayBridge {
  rpc Connect(stream GatewayToMedia) returns (stream MediaToGateway);
}

message GatewayToMedia {
  oneof body {
    DeviceHello        device_hello         = 1;
    ConfigUpdate       config_update        = 2;
    AudioFrame         audio_frame          = 3;  // G.711A 字节
    AudioBoundary      audio_boundary       = 4;
    FunctionCallOutput function_call_output = 5;
    Bye                bye                  = 6;
    Ping               ping                 = 7;
  }
  string device_id  = 100;
  string session_id = 101;
}

message MediaToGateway {
  oneof body {
    HelloAck           hello_ack            = 1;
    HelloErr           hello_err            = 2;
    Status             status               = 3;
    Event              event                = 4;
    Text               text                 = 5;
    FunctionCall       function_call        = 6;
    ConfigUpdateAck    config_update_ack    = 7;
    ConfigUpdateErr    config_update_err    = 8;
    AudioChunk         audio_chunk          = 9;  // TTS 合成音频
    Pong               pong                 = 10;
  }
}
```

---

## 📑 Slide 18 — 接口规范：MediaService ↔ AITalk

```protobuf
syntax = "proto3";
package aitalk.v1;

service AuthService {
  rpc Authenticate(AuthRequest) returns (AuthResponse);
}

service TalkService {
  rpc Connect(stream MediaToAi) returns (stream AiToMedia);
}

message MediaToAi {     // MediaService → AITalk
  oneof body {
    AsrFinal           asr_final           = 1;  // ASR 文字
    AsrPartial         asr_partial         = 2;
    VadEvent           vad_event           = 3;
    ConfigUpdate       config_update       = 4;
    FunctionCallOutput function_call_output = 5;
    Bye                bye                 = 6;
    Ping               ping                = 7;
  }
  string session_id = 100;
}

message AiToMedia {     // AITalk → MediaService
  oneof body {
    Status           status            = 1;
    Event            event             = 2;
    TextDelta        text_delta        = 3;
    FunctionCall     function_call     = 4;
    TtsText          tts_text          = 5;  // TTS 文字
    ConfigUpdateAck  config_update_ack = 6;
    ConfigUpdateErr  config_update_err = 7;
    Pong             pong              = 8;
  }
}
```

**关键**：这条流里**没有任何音频字节**。

---

## 📑 Slide 19 — 完整数据流（一个回合）

```
设备       Gateway          MediaService          AITalk           LLM
 │           │                    │                    │              │
 │─Hello────▶│                   │                    │              │
 │           │─Auth gRPC─────────────────────────────────▶           │
 │           │◀─JWT──────────────────────────────────────           │
 │◀─hello_ack│                   │                    │              │
 │           │─gRPC stream open─▶                    │              │
 │           │                    │─Auth gRPC───────────────────────▶│
 │           │                    │◀─JWT────────────────────────────│
 │           │                    │─gRPC stream open────────────────▶│
 │           │                    │                    │              │
 │─Audio BIN─▶                   │                    │              │
 │           │─AudioChunk gRPC──▶                    │              │
 │           │                    │─denoise+vad+asr    │              │
 │           │                    │─AsrFinal "今天…→"  │              │
 │           │                    │─────────────────▶│              │
 │           │                    │                    │─LLM stream──▶│
 │           │                    │                    │◀─text_delta─│
 │           │◀─text─────────────│                    │              │
 │           │                    │◀─TtsText──────────│              │
 │           │                    │─tts               │              │
 │           │                    │◀─AudioChunk────────│              │
 │           │◀─Audio BIN────────│                    │              │
 │           │                    │◀─FC emotion───────│              │
 │           │◀─func_call────────│                    │              │
 │           │                    │                    │              │
 │◀─Audio BIN────────────────────                    │              │
 │◀─status ans│                   │                    │              │
```

---

## 📑 Slide 20 — JWT 流转

```
设备 hello
   ↓
Gateway (不持有设备数据库) → 透传给 MediaService
   ↓
MediaService → AITalk.AuthService.Authenticate
   │
   ▼
JWT (24h expiry)
   │
   ├── Gateway 缓存，用于后续 gRPC stream
   │
   └── MediaService 也缓存，校验 aud 字段
       (aud="aitalk" 才能接受 Gateway 的 stream)
```

JWT claim 结构：
```json
{
  "device_id":     "d_abc123",
  "product_id":    "p_xxx",
  "session_uuid":  "s_yyy",
  "audio_config":  { "codec": "g711a", "sample_rate_hz": 8000 },
  "exp":           1737034800,
  "iss":           "aitalk",
  "aud":           "aitalk"
}
```

---

## 📑 Slide 21 — 部署拓扑

```
[设备]
  │ 公网 (WSS :9443)
  ▼
[CloudFront] → [envoy]
  │ 4 层 LB，TLS 终结
  │
  ├──▶ Gateway pool (5-10 实例, CPU 节点)
  │       │ gRPC
  │
  ├──▶ MediaService pool (5-10 实例, GPU+CPU 混合节点)
  │       │ gRPC
  │
  └──▶ AITalk pool (8-20 实例, CPU 节点)
          │
          └─▶ HTTPS ─▶ 外部 AI (OpenAI / Qwen / 文心)
```

---

## 📑 Slide 22 — 资源清单

| 服务 | 节点类型 | 单实例规模 | 总规模 |
|---|---|---|---|
| envoy | CPU | 2 核 4 GB | 2-3 实例 |
| Gateway | CPU | 4 核 4 GB | 5-10 实例 |
| MediaService | GPU + CPU | 12 核 18 GB + 1×A10 | 5-10 实例 |
| AITalk | CPU | 8 核 16 GB | 8-20 实例 |
| PostgreSQL | CPU + SSD | 16 核 32 GB | 2 实例主从 |
| Redis | CPU + 内存 | 8 核 16 GB | 3 实例集群 |

---

## 📑 Slide 23 — 故障隔离矩阵

| 故障 | Gateway | MediaService | AITalk |
|---|---|---|---|
| Gateway 挂 | LB 切走 | ✅ 不影响 | ✅ 不影响 |
| MediaService 挂 | 重连其他 | LB 切走 | ✅ 不影响（text-only 降级） |
| AITalk 挂 | ✅ 正常 | 重连其他 | LB 切走 |
| LLM 厂商宕机 | ✅ 正常 | ✅ 正常 | 切备用 LLM |
| ASR 厂商宕机 | ✅ 正常 | 内部切备用 | ✅ 不影响 |
| TTS 厂商宕机 | ✅ 正常 | 内部切备用 | ✅ 不影响 |

---

## 📑 Slide 24 — 监控指标

| 服务 | 关键指标 |
|---|---|
| **Gateway** | `active_connections` / `messages_in_total{type}` / `messages_out_total{type}` / `ratelimit_hits_total` |
| **MediaService** | `asr_queue_depth` / `asr_latency_ms_p95` / `tts_first_chunk_ms_p95` / `vad_denoise_queue_depth` |
| **AITalk** | `active_sessions` / `llm_latency_ms_p95` / `llm_first_token_ms_p95` / `tool_call_total{name,executor}` / `tokens_total{direction}` |

---

## 📑 Slide 25 — MediaService 子模块拆分演进

| 阶段 | 描述 | 触发条件 |
|---|---|---|
| Phase A | 三子模块同进程（默认） | 启动简单 |
| Phase B | vad_denoise 拆出 | vad 队列深度持续 > 100 |
| Phase C | asr_engine 拆出 | GPU 利用率持续 > 80% |
| Phase D | tts_engine 拆出 | TTS 高峰时段需要独立扩缩 |

**默认三合一**（降低运维复杂度），瓶颈时再拆分。

---

## 📑 Slide 26 — 演进路线

| 阶段 | 内容 | 何时 |
|---|---|---|
| Phase 0 | 单体 cloud_gateway（含 stub 后端） | ✅ 已完成 |
| Phase 1 | cloud_gateway + 真实 ASR/LLM/TTS（替换 stub） | 1-2 周 |
| Phase 2 | 拆 MediaService 进程（GPU 节点独立扩缩） | 2-3 周 |
| Phase 3 | 拆 AITalk 业务 | 2-3 周 |
| Phase 4 | K8s + 服务网格 + 多 region | 1-2 周 |

---

## 📑 Slide 27 — 关键设计决策

| 决策 | 选择 | 理由 |
|---|---|---|
| 调用方向 | Gateway → MediaService → AITalk | 音频 AI 在中间层自然存在 |
| MediaService 位置 | Gateway 和 AITalk 之间 | 边界最清晰 |
| ASR/TTS 拆分 | MediaService 内部合一，必要时拆 | 起步简单 |
| System prompt | AITalk 重新拼装（不信任上游） | 防篡改 |
| FC 路由 | AITalk 决定 executor | 业务策略集中 |
| 音频字节边界 | 不出 MediaService | 节省 100x 带宽 |
| 鉴权 | AITalk 颁发 JWT，Gateway/MediaService 缓存 | 单点认证 |
| session 状态 | Redis 外置 | AITalk 实例可无状态化 |

---

## 📑 Slide 28 — 编译时解耦矩阵

| 服务 | 语言 | 团队 | 变更频率 |
|---|---|---|---|
| Gateway | C++ | 协议组 | 业务迭代，**频繁改** |
| MediaService | Python | AI 平台组 | AI 模型迭代，**中等** |
| AITalk | Go/Python | 业务组 | 业务编排，**中等** |

- Gateway 改 AITalk 同步发布
- MediaService 改 AITalk 不感知（接口稳定即可）
- Gateway 改 MediaService 不感知（接口稳定即可）
- 三者用不同语言，**互不 block**

---

## 📑 Slide 29 — 总结（一句话）

```
Gateway      = 设备翻译官（WS ↔ gRPC）
MediaService = 音频 AI 一条龙（denoise+vad+asr+tts）
AITalk       = 业务大脑（LLM + 配置 + FC + 状态）
```

调用方向：**Gateway → MediaService → AITalk**  
协议分层：WSS（公网） + gRPC（内网）  
关键边界：**音频字节永远不出 MediaService 边界** → 网络最优、职责最清、故障最小。

---

## 📑 Slide 30 — 详细文档导航

| 服务 | 设计文档 |
|---|---|
| 网关 | [gateway/gateway_design.md](gateway/gateway_design.md) |
| 音频 AI 层 | [media_service/media_service_design.md](media_service/media_service_design.md) |
| 业务编排 | [ai_talk/ai_talk_design.md](ai_talk/ai_talk_design.md) |

| 相关文档 | 用途 |
|---|---|
| [README.md](README.md) | 整体架构总览 |
| [../cloud_business_design.md](../cloud_business_design.md) | 单体版设计（被本架构取代） |
| [../settings_app_logic.md](../settings_app_logic.md) | 设备端 settings app 业务分析 |
| [../protocol.md](../protocol.md) | WS 协议规范 |
| [../architecture.md](../architecture.md) | cloud_gateway 单体版架构 |

---

## 📑 附录：导出为 PPTX 的命令

如果需要导出为真正的 PowerPoint 文件：

```bash
# 安装 pandoc（如果未安装）
# Ubuntu: apt install pandoc
# macOS: brew install pandoc
# Windows: choco install pandoc

# 转换
pandoc architecture_presentation.md -o architecture_presentation.pptx \
  --slide-level=1 \
  -V theme=Berlin

# 或使用 Reveal.js / Marp 等 Markdown → 幻灯片工具
```

> **提示**：每张幻灯片以 `## 📑 Slide N` 开头，使用 pandoc 默认行为会自动按标题分页。

---

> 文档结束。架构设计 PPT 完成。
>
> **下一步**：根据此 PPT 组织架构评审会，与团队对齐边界；启动 Phase 1 实施（真实 ASR/LLM/TTS 替换 stub）。