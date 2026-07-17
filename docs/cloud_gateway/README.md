# 整体架构图与微服务关系

> 调用方向：**Gateway → MediaService → AITalk**  
> **Gateway ↔ MediaService 通过 WebSocket + JWT**（复用 Gateway 的 WS 代码）  
> **MediaService ↔ AITalk 通过 gRPC**（AI 编排层用强类型）  
> **Gateway 不直接连 AITalk**，所有指令经 MediaService 中转

---

## 1. 整体架构图（最终版）

```
                                                                    
                       设备 (WS63 嵌入式)                                  
                            │                                           
                            │ WSS convai.v1                            
                            │ (公网入口，TLS 终结在 cloudfront/envoy)  
                            ▼                                           
                  ┌─────────────────────────────────────┐                  
                  │      Nginx / envoy                  │  ← 边缘 LB + TLS
                  │      (L7 负载均衡 + JWT 校验)       │     终结
                  └────────────────┬────────────────────┘                 
                                   │                                        
                  ┌────────────────┴────────────────────────────────┐       
                  │  Gateway 服务池（5-10 实例，C++17）             │       
                  │  • WS 协议终结 + 鉴权 + 业务转发                │       
                  │  • 入：WSS convai.v1（设备）                    │       
                  │  • 出：WS+JWT convai.media.v1（→ MediaService） │       
                  └────────────────┬───────────────────────────────┘       
                                   │ WebSocket + JWT（内网）
                                   │ 子协议：convai.media.v1
                                   │ 透传：设备消息 + G.711A 字节
                                   ▼                                       
                  ┌─────────────────────────────────────┐                  
                  │  MediaService 服务池（5-10 实例）     │  ← 音频 AI 层
                  │  ┌────────────────────────────────┐  │       
                  │  │ 内部子模块：                     │  │       
                  │  │  • vad_denoise (CPU 轻量)        │  │       
                  │  │  • asr_engine (GPU 重活)         │  │       
                  │  │  • tts_engine (GPU 重活)         │  │       
                  │  │  • 编排：denoise→vad→asr→文字    │  │       
                  │  │         文字→tts→音频              │  │       
                  │  └────────────────────────────────┘  │       
                  └────────────────┬────────────────────┘                  
                                   │ gRPC（内网）                     
                                   │ 只传文字 + 控制消息            
                                   ▼                                       
                  ┌─────────────────────────────────────┐                  
                  │  AITalk 服务池（8-20 实例）         │  ← 业务编排层
                  │  ┌────────────────────────────────┐  │       
                  │  │  • LLM 推理（GPT-4o/Qwen/文心）│  │       
                  │  │  • Function Calling 路由       │  │       
                  │  │  • 5 维配置管理                 │  │       
                  │  │  • 会话状态机                   │  │       
                  │  │  • 对话历史                     │  │       
                  │  └────────────────────────────────┘  │       
                  └─────────────────────────────────────┘                  
```

---

## 2. 调用方向（自顶向下）

```
设备 (WSS convai.v1)
   │
   ▼
Gateway (WSS convai.v1 ←设备 / WS+JWT convai.media.v1 → MediaService)
   │
   ▼
MediaService (WS+JWT ←Gateway / gRPC → AITalk)
   │
   ▼
AITalk (gRPC ←MediaService / HTTPS → LLM)
```

> **关键**：Gateway 不直接连 AITalk。**所有指令和音频都经 MediaService 中转**。

---

## 3. 三层服务定位

| 层级 | 服务 | 职责一句话 | 资源特征 |
|---|---|---|---|
| **入口层** | Gateway | 设备 WS 协议终结 + 鉴权 + 路由 | CPU + 网络 IO |
| **音频 AI 层** | MediaService | 把音频变文字（ASR pipeline）+ 把文字变音频（TTS pipeline） | CPU + GPU（混合） |
| **业务编排层** | AITalk | 拿到文字后决定回复什么（LLM + 配置 + FC + 状态） | CPU |

---

## 4. 三段链路详解

### 4.1 链路 A：设备 → Gateway

```
设备 (WS63)
    │ WS upgrade (WSS convai.v1)
    │ TLS 终结于 cloudfront/envoy（公网）
    ▼
Gateway (C++, epoll)
    • 解析 HTTP Upgrade 请求
    • 调 MediaService.AuthService（由 MediaService 转发到 AITalk）
    • MediaService 返 JWT（24h expiry）
    • Gateway 缓存 JWT，通过则回 101 Switching Protocols
    • 此后所有 WS 帧进入业务流
```

**关键**：
- Gateway 不持有设备数据库，只转 hello 给 MediaService
- 凭证校验链路：Gateway → MediaService → AITalk

### 4.2 链路 B：Gateway → MediaService（核心：WebSocket + JWT）

```
Gateway 收到设备的 WS 帧
    │
    │ WebSocket + JWT（内网）
    │ 子协议：convai.media.v1
    │ URL：ws://media-lb.internal:50050/ingest
    │
    ▼
MediaService
    • 接收 hello / config_update / 音频帧 / function_call_output
    • 自己做：denoise → vad → asr → 文字
    • 文字 + 控制消息 → gRPC 转给 AITalk
    • 接收 AITalk 的 TTS 请求 → 合成音频
    • 音频 → 通过 WS → 回到 Gateway
    │
    │ Gateway 序列化为 WS 帧 → 设备
```

**为什么用 WebSocket 而不是 gRPC？**

| 原因 | 说明 |
|---|---|
| **代码复用** | Gateway 是 C++，已实现 WS 协议终结，复用现有代码 |
| **跨语言友好** | MediaService 是 Python，WS 服务器简单（`websockets` 库一行起服务） |
| **双工流** | WebSocket 是文本/二进制双工，长连接友好 |
| **JWT 鉴权** | 放在 WS 握手 Header 中（`Sec-WebSocket-Auth: Bearer <JWT>`） |
| **nginx 友好** | MediaService 前置 nginx 做 L7 LB，配置简单 |

**MediaService 同时做四件事**：
1. 接收 Gateway 转发的设备消息（含音频字节）
2. 自主完成 denoise + vad + asr（音频 → 文字）
3. 把文字 + 控制消息透传给 AITalk（经 gRPC）
4. 接收 AITalk 返回的 TTS 音频，逆向回给 Gateway（经 WS）

### 4.3 链路 C：MediaService → AITalk（gRPC）

```
MediaService 拿到 ASR 文字（VAD 边界检测后触发）
    │
    │ gRPC stream（内网，mTLS 可选）
    │ 协议：aitalk.v1.TalkService
    │
    ▼
AITalk
    • 拼装 LLM prompt（system_messages + history）
    • 调 LLM（GPT-4o / Qwen / 文心）
    • 流式接收 text_delta + tool_calls
    • Function Calling 路由（透传设备 vs 云端工具）
    • 触发 MediaService.Synthesize（TTS）
    │
    │ 把 TTS 文字发给 MediaService
    │ 接收回 TTS 音频
```

**MediaService 与 AITalk 之间只传两类数据**：
- 上行：ASR 文字（已不含音频）
- 下行：TTS 文字（AITalk 输出文字，MediaService 合成音频）

→ **音频字节永远不出 MediaService 的边界**，这对带宽和延迟优化都有利。

---

## 5. 三服务角色对比

| 服务 | 上下游 | 主要职责 | 暴露协议 | 资源 |
|---|---|---|---|---|
| **Gateway** | 设备 ↔ MediaService | WS 协议终结 + 鉴权 + 字节流转发 | 入：WSS(设备) / 出：WS+JWT(→MediaService) | CPU + 网络 |
| **MediaService** | Gateway ↔ AITalk | 音频 AI 一条龙（denoise+vad+asr+tts+format） | 入：WS+JWT(←Gateway) / 出：gRPC(→AITalk) | CPU + GPU 混合 |
| **AITalk** | MediaService / LLM | 业务编排 + LLM + 配置 + FC + 状态 | 入：gRPC(←MediaService) / 出：HTTPS(→LLM) | CPU |

---

## 6. 为什么这个拓扑？

### 6.1 边界自然

```
设备字节流 ──[WS]──▶ Gateway ──[WS+JWT]──▶ MediaService ──[gRPC]──▶ AITalk
```

- **Gateway** 不关心音频算法 → 只透传
- **AITalk** 不关心音频算法 → 只拿文字
- **MediaService** 是唯一关心"音频字节"的层 → 自然在这两者之间

### 6.2 网络带宽优化

- 设备 ← Gateway：WS 携带 G.711A（公网 1 GB/s 峰值）
- Gateway ← MediaService：WS 携带 G.711A（内网，万兆）
- MediaService ← AITalk：gRPC 携带**文字**（KB/s 量级，省 100x 带宽）

### 6.3 故障隔离

| 故障 | Gateway | MediaService | AITalk |
|---|---|---|---|
| Gateway 挂 | LB 切走 | ✅ 不影响 | ✅ 不影响 |
| MediaService 挂 | 重连其他 | LB 切走 | ✅ 不影响（text-only 降级） |
| AITalk 挂 | ✅ 正常 | 重连其他 | LB 切走 |
| LLM 厂商宕机 | ✅ 正常 | ✅ 正常 | 切备用 LLM |
| ASR 厂商宕机 | ✅ 正常 | 内部切备用 | ✅ 不影响 |
| TTS 厂商宕机 | ✅ 正常 | 内部切备用 | ✅ 不影响 |

### 6.4 编译时解耦

| 服务 | 语言 | 团队 | 变更频率 |
|---|---|---|---|
| Gateway | C++ | 协议组 | 业务迭代，**频繁改** |
| MediaService | Python | AI 平台组 | AI 模型迭代，**中等** |
| AITalk | Go/Python | 业务组 | 业务编排，**中等** |

MediaService 改 AI 模型（Whisper v3 → v4），AITalk 完全无感。
AITalk 改 LLM SDK（OpenAI → 自研），MediaService 完全无感。
Gateway 改设备协议（convai.v1 → v2），MediaService 只需要改 WS 握手校验。

---

## 7. 协议规范

### 7.1 Gateway ↔ 设备：WebSocket `convai.v1`

| 帧类型 | 方向 | 用途 |
|---|---|---|
| TEXT hello | 设备 → | 鉴权 |
| TEXT config_update | 设备 → | 5 维配置变更 |
| TEXT function_call_output | 设备 → | 应答云端 FC |
| BIN AudioFrame | 设备 → | G.711A 20ms |
| TEXT hello_ack | → 设备 | 会话建立 |
| TEXT status | → 设备 | 6 种状态 |
| TEXT text | → 设备 | AI 文字回复 |
| TEXT function_call | → 设备 | 云端 FC 透传 |
| BIN AudioFrame | → 设备 | TTS 合成 |

### 7.2 Gateway ↔ MediaService：WebSocket `convai.media.v1`

**鉴权**（在 WS 握手 Header 中）：
```
GET /ingest HTTP/1.1
Host: media-lb.internal:50050
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Key: <random>
Sec-WebSocket-Version: 13
Sec-WebSocket-Protocol: convai.media.v1
Sec-WebSocket-Auth: Bearer <JWT>     ← 设备 hello 后 AITalk 颁发的 JWT
```

**帧类型**（TEXT，JSON）：
```json
// Gateway → MediaService
{"type":"device_hello", "device_id":"...", "product_id":"...", ...}
{"type":"config_update", "body":{...}}
{"type":"audio_frame", "g711a_data":"<base64>", "ts_ms":..., "session_id":"..."}
{"type":"audio_boundary", "event":"speech_start", "session_id":"..."}
{"type":"function_call_output", "call_id":"...", "output":"..."}
{"type":"bye", "session_id":"..."}

// MediaService → Gateway
{"type":"hello_ack", "device_id":"..."}
{"type":"status", "status":"listening", "session_id":"..."}
{"type":"text", "content":"好呀！", "session_id":"..."}
{"type":"function_call", "name":"emotion", "arguments":{...}, "session_id":"..."}
{"type":"audio_chunk", "g711a_data":"<base64>", "ts_ms":..., "session_id":"..."}
```

**注**：音频字节用 base64 编码（避免 JSON 转义复杂度），MediaService 端解码后做 ASR。

### 7.3 MediaService ↔ AITalk：gRPC `aitalk.v1.TalkService`

**关键**：这条流里**没有任何音频字节**——只有文字 + 控制消息。

```protobuf
message MediaToAi {
  oneof body {
    AsrFinal           asr_final           = 1;  // ASR 文字
    AsrPartial         asr_partial         = 2;
    VadEvent           vad_event           = 3;
    ConfigUpdate       config_update       = 4;
    FunctionCallOutput function_call_output = 5;
    Bye                bye                 = 6;
  }
}

message AiToMedia {
  oneof body {
    Status           status            = 1;
    Event            event             = 2;
    TextDelta        text_delta        = 3;
    FunctionCall     function_call     = 4;
    TtsText          tts_text          = 5;  // TTS 文字
    ConfigUpdateAck  config_update_ack = 6;
  }
}
```

---

## 8. 完整数据流（一个回合）

```
设备       Gateway           MediaService              AITalk           LLM
 │           │                    │                       │              │
 │─Hello────▶│                   │                       │              │
 │           │─WS convai.media.v1 (WS+JWT) ─────────▶                │
 │           │  hello (product_id/key/secret)         │              │
 │           │                    │─Auth (转 AITalk)────▶│              │
 │           │                    │◀─JWT─────────────────│              │
 │           │◀─hello_ack─────────│                       │              │
 │           │                    │─gRPC stream open────▶│              │
 │           │                    │─AsrFinal "今天…学画画"─▶            │
 │           │                    │                       │              │
 │─Audio BIN─▶                   │                       │              │
 │           │─WS audio_frame────▶                       │              │
 │           │                    │─denoise+vad+asr     │              │
 │           │                    │─AsrFinal "今天学画画"─▶            │
 │           │                    │                       │              │
 │           │                    │                       │─LLM stream──▶│
 │           │                    │◀─TextDelta "好呀！"──────────────────│
 │           │◀─text─────────────│                       │              │
 │           │                    │◀─TtsText "好呀！"──────────────────│
 │           │                    │─tts 合成              │              │
 │           │                    │◀─AudioChunk (G.711A)───────────────│
 │           │◀─Audio BIN (TTS)──│                       │              │
 │           │                    │◀─FC emotion=happy─────────────────│
 │           │◀─func_call────────│                       │              │
 │           │                    │                       │              │
 │─FC_output─▶                   │                       │              │
 │           │─WS FC_output─────▶                       │              │
 │           │                    │                       │              │
 │◀─status ans│                   │                       │              │
 │◀─status fin│                   │                       │              │
 │◀─status lst│                   │                       │              │
```

---

## 9. 部署拓扑

```
[设备]
  │ 公网 (WSS :9443)
  ▼
[CloudFront] → [envoy]
  │
  ├──▶ Gateway pool (5-10 实例, CPU 节点)
  │       │ WebSocket + JWT (内网 :50050)
  │
  ├──▶ MediaService pool (5-10 实例, GPU+CPU 混合节点)
  │       │ gRPC (内网 :50051)
  │
  └──▶ AITalk pool (8-20 实例, CPU 节点)
          │
          └─▶ HTTPS ─▶ 外部 AI (OpenAI / Qwen / 文心)
```

---

## 10. 资源清单

| 服务 | 节点类型 | 单实例规模 | 总规模 |
|---|---|---|---|
| envoy | CPU | 2 核 4 GB | 2-3 实例 |
| Gateway | CPU | 4 核 4 GB | 5-10 实例 |
| MediaService | GPU + CPU | 12 核 18 GB + 1×A10 | 5-10 实例 |
| AITalk | CPU | 8 核 16 GB | 8-20 实例 |
| PostgreSQL | CPU + SSD | 16 核 32 GB | 2 实例主从 |
| Redis | CPU + 内存 | 8 核 16 GB | 3 实例集群 |

---

## 11. 故障隔离矩阵

| 故障 | Gateway | MediaService | AITalk |
|---|---|---|---|
| Gateway 挂 | LB 切走 | ✅ 不影响 | ✅ 不影响 |
| MediaService 挂 | 重连其他 | LB 切走 | ✅ 不影响（text-only 降级） |
| AITalk 挂 | ✅ 正常 | 重连其他 | LB 切走 |
| LLM 厂商宕机 | ✅ 正常 | ✅ 正常 | 切备用 LLM |
| ASR 厂商宕机 | ✅ 正常 | 内部切备用 | ✅ 不影响 |
| TTS 厂商宕机 | ✅ 正常 | 内部切备用 | ✅ 不影响 |

---

## 12. MediaService 内部三个子能力

虽然 MediaService 是一个进程，但内部可清晰分为三块：

| 子能力 | 资源 | 实现 | 可拆分性 |
|---|---|---|---|
| **vad_denoise** | CPU 轻量 | Silero VAD + RNNoise | 流量大时拆出独立服务 |
| **asr_engine** | GPU 重 | Whisper / iFlytek | GPU 独立扩缩容时拆出 |
| **tts_engine** | GPU 重 | Volcano / CosyVoice | GPU 独立扩缩容时拆出 |

**默认三合一部署**（降低运维复杂度），当某个子能力成为瓶颈时，再独立拆分。

---

## 13. 详细文档导航

- 网关：[gateway/gateway_design.md](gateway/gateway_design.md)
- 音频 AI 层：[media_service/media_service_design.md](media_service/media_service_design.md)
- 业务编排：[ai_talk/ai_talk_design.md](ai_talk/ai_talk_design.md)

---

## 14. 总结

**最终调用方向**：

```
设备 → Gateway → MediaService → AITalk
                  (WS+JWT)    (gRPC)
```

**关键边界**：

| 边界 | 数据形式 | 带宽 |
|---|---|---|
| 设备 ↔ Gateway | WS 原始字节流（公网） | GB/s |
| Gateway ↔ MediaService | **WS+JWT**（内网，audio 用 base64） | 万兆 |
| MediaService ↔ AITalk | gRPC **只传文字**（内网） | KB/s |

**角色一句话**：

> **Gateway** = 设备翻译官（WS ↔ WS+JWT）  
> **MediaService** = 音频 AI 一条龙（denoise+vad+asr+tts）  
> **AITalk** = 业务大脑（LLM + 配置 + FC + 状态）

**关键约束**：**Gateway 不直接连 AITalk**，所有指令和音频都经 MediaService 中转。  
音频字节永远不出 MediaService 边界 → 网络最优、职责最清、故障最小。