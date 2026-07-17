# MediaService 服务设计

> 服务代号：`media-service`  
> 位置：**Gateway 与 AITalk 之间**  
> 上下游：上接 Gateway（设备消息 + 音频字节），下接 AITalk（仅文字事件）  
> 资源特征：**CPU + GPU 混合**，内含 denoise + vad + asr + tts 四条子流水线

---

## 1. 单一职责

> **MediaService 是一条"音频 AI 一条龙"——把设备发来的 G.711A 音频流变成文字事件，把 AITalk 给的文字变成 G.711A 音频流。**

具体：
- 音频前端处理：**降噪**（RNNoise / 谱减法） + **VAD**（Silero VAD）
- 语音识别（**ASR**）：Whisper / iFlytek / Paraformer
- 语音合成（**TTS**）：Volcano / CosyVoice / Edge
- 音频格式转换：G.711A ↔ PCM16 ↔ Opus

## 2. 不做的事

- ❌ 设备协议（WS convai.v1）→ 交给 Gateway
- ❌ LLM 调用 → 交给 AITalk
- ❌ Function Calling 路由 → 交给 AITalk
- ❌ 会话状态机 → 交给 AITalk
- ❌ 设备鉴权 / 配置管理 → 交给 AITalk

## 3. 架构（MediaService 在 Gateway ↔ AITalk 之间）

```
                  
   Gateway                    MediaService                     AITalk
     │                              │                              │
     │─[gRPC: GatewayBridge]────────▶│                              │
     │   设备 hello / config_update │                              │
     │   音频帧 (G.711A 字节)        │                              │
     │                              │─[denoise + vad + asr]        │
     │                              │─[AsrFinal "今天我们学画画"]──▶│
     │                              │                              │
     │                              │◀─[TtsText "好呀！"]─────────│
     │                              │─[tts (Volcano)]             │
     │◀─[AudioChunk G.711A]──────────│                              │
     │                              │◀─[text_delta]────────────────│
     │                              │◀─[FC emotion=happy]──────────│
     │                              │                              │
```

**关键**：MediaService 与 AITalk 之间只传文字（KB/s 量级），不传音频字节（MB/s 量级）。  
这节省了 100x 的内部带宽，也隔离了 AITalk 的 CPU。

---

## 4. 接口规范

### 4.1 上游：Gateway → MediaService（gRPC）

```protobuf
syntax = "proto3";
package media.v1;

service GatewayBridge {
  // 双向流：设备消息 ↔ AITalk消息（经 MediaService 转发/翻译）
  rpc Connect(stream GatewayToMedia) returns (stream MediaToGateway);
}

message GatewayToMedia {
  oneof body {
    DeviceHello        device_hello         = 1;
    ConfigUpdate       config_update        = 2;
    AudioFrame         audio_frame          = 3;  // 原始 G.711A 字节
    AudioBoundary      audio_boundary       = 4;  // VAD start/end（来自设备）
    FunctionCallOutput function_call_output = 5;
    Bye                bye                  = 6;
    Ping               ping                 = 7;
  }
  string device_id  = 100;
  string session_id = 101;
  uint32 seq         = 102;
  uint64 ts_ms       = 103;
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
    AudioChunk         audio_chunk          = 9;  // TTS 合成的 G.711A
    Pong               pong                 = 10;
  }
  uint32 seq   = 102;
  uint64 ts_ms = 103;
}
```

### 4.2 下游：MediaService → AITalk（gRPC）

```protobuf
syntax = "proto3";
package aitalk.v1;

service TalkService {
  rpc Connect(stream MediaToAi) returns (stream AiToMedia);
}

message MediaToAi {
  oneof body {
    AsrFinal           asr_final           = 1;   // ASR 最终文字
    AsrPartial         asr_partial         = 2;   // ASR 中间文字（可选）
    VadEvent           vad_event           = 3;   // SPEECH_START / SPEECH_END
    ConfigUpdate       config_update       = 4;
    FunctionCallOutput function_call_output = 5;
    Bye                bye                 = 6;
    Ping               ping                = 7;
  }
  string session_id = 100;
  uint32 seq         = 101;
  uint64 ts_ms       = 102;
}

message AiToMedia {
  oneof body {
    Status           status            = 1;
    Event            event             = 2;
    TextDelta        text_delta        = 3;
    FunctionCall     function_call     = 4;
    TtsText          tts_text          = 5;   // 需要 TTS 合成的文字
    ConfigUpdateAck  config_update_ack = 6;
    ConfigUpdateErr  config_update_err = 7;
    Pong             pong              = 8;
  }
  string session_id = 100;
  uint32 seq         = 101;
  uint64 ts_ms       = 102;
}
```

**关键差异**：
- Gateway ↔ MediaService：包含 `AudioFrame` 和 `AudioChunk`（G.711A 字节）
- MediaService ↔ AITalk：**只有文字**，没有任何音频字节

---

## 5. 内部架构

```
MediaService 实例 (Python)
├── GatewayBridge 模块
│     • 接收 GatewayToMedia（设备消息 + 音频字节）
│     • 解码 hello / config_update → 转发给 AITalk
│     • 接收 AudioFrame → 上行 ASR pipeline
│     • 接收 AiToMedia 中的 AudioChunk → 编码 WS 帧 → 回给 Gateway
│
├── TalkService 模块
│     • 与 AITalk 维持 1 个双向 gRPC stream
│     • 上行：把 AsrFinal / VadEvent 推给 AITalk
│     • 下行：把 AITalk 的 TextDelta / TtsText 拉回来
│
├── PipelineOrchestrator（核心）
│     │
│     ├─[上行 pipeline: AudioFrame → AsrFinal]
│     │   AudioFrame → vad_denoise → asr_engine → AsrPartial/Final
│     │
│     └─[下行 pipeline: TtsText → AudioChunk]
│         TtsText → tts_engine → AudioChunk (流式)
│
└── 子模块（可独立替换或拆分）
      ├── vad_denoise
      ├── asr_engine
      └── tts_engine
```

---

## 6. 上行 ASR Pipeline（音频 → 文字）

### 6.1 数据流

```
[Gateway] AudioFrame (160B G.711A + 13B 头)
    │
    ▼
[MediaService.GatewayBridge] 累积到 per-session ring buffer
    │
    ▼
[vad_denoise]
    • 输入：流式音频
    • 1) 降噪：RNNoise / 谱减法（每 20ms 一帧）
    • 2) VAD：Silero VAD 检测
    • 3) 输出：
        - VAD 检测到 speech_start → 通知 PipelineOrchestrator 开始 ASR 回合
        - VAD 检测到 speech_end → 通知 ASR 引擎停止
        - 持续：denoised audio（送给 asr_engine）
    │
    ▼
[asr_engine]
    • 输入：denoised 音频（从 speech_start 到 speech_end）
    • Whisper-large-v3 / iFlytek 流式推理
    • 输出：
        - AsrPartial("今天我们")  ← 边收边识别
        - AsrPartial("今天我们学")
        - AsrFinal("今天我们学画画")
    │
    ▼
[MediaService.TalkService] → 发给 AITalk
```

### 6.2 内部时序（一个回合）

```
[T=0ms]    Gateway 发 AudioFrame #1
[T=20ms]   Gateway 发 AudioFrame #2
...
[T=100ms]  vad_denoise 检测到 speech_start
[T=100ms]  PipelineOrchestrator 启动 asr_engine 流
[T=200ms]  asr 返回 AsrPartial "今天我们"
[T=300ms]  asr 返回 AsrPartial "今天我们学"
[T=400ms]  asr 返回 AsrFinal "今天我们学画画"
[T=420ms]  vad_denoise 检测到 speech_end
[T=425ms]  PipelineOrchestrator 关闭 asr_engine 流
[T=430ms]  TalkService 发 AsrFinal → AITalk
```

**端到端延迟**（VAD end → AsrFinal）：~150-300 ms

---

## 7. 下行 TTS Pipeline（文字 → 音频）

### 7.1 数据流

```
[AITalk] TtsText ("好呀！")
    │
    ▼
[MediaService.TalkService] 接收
    │
    ▼
[PipelineOrchestrator] 把 TtsText 提交给 tts_engine
    │
    ▼
[tts_engine]
    • 输入：text + voice_type
    • Volcano / CosyVoice 流式合成
    • 输出：AudioChunk (20ms G.711A) 流式返回
    │
    ▼
[MediaService.GatewayBridge] 序列化 WS 帧
    │
    ▼
[Gateway] 收到 AudioChunk → 转 WS AudioFrame → 设备
```

### 7.2 流式输出

```
tts_engine 流式返回：
  AudioChunk(seq=1, audio_data=160B)
  AudioChunk(seq=2, audio_data=160B)
  ...
  AudioChunk(seq=N, audio_data=160B, is_last=true)

每个 AudioChunk 立即经 Gateway 推到设备的 WS。
设备 ring buffer 累积 → 播放器取出 → 扬声器。
```

---

## 8. 三个子能力详细说明

### 8.1 vad_denoise（音频前端）

| 维度 | 规格 |
|---|---|
| 资源 | CPU 轻量（每实例 1-2 核） |
| 延迟 | < 30 ms |
| 模型 | Silero VAD（5MB，CPU 推理）+ RNNoise（小型 RNN，降噪） |
| 输出 | VadEvent(SPEECH_START/END) + denoised_g711a |

接口（内部 Python）：
```python
class VadDenoise:
    def process_frame(self, g711a_bytes: bytes) -> VadEvent:
        pcm = g711a_to_pcm(g711a_bytes)
        denoised = rnnoise.process(pcm)
        event = silero_vad.detect(denoised)
        return event, g711a_from_pcm(denoised)
```

### 8.2 asr_engine（语音识别）

| 维度 | 规格 |
|---|---|
| 资源 | GPU 重（每实例 12 核 + 1×A10） |
| 延迟 | 200-1500 ms（含网络） |
| 模型 | Whisper-large-v3 / iFlytek 流式 / Paraformer |
| 输出 | AsrPartial（流） + AsrFinal（结束） |

接口（内部 Python）：
```python
class AsrEngine:
    def recognize(self, denoised_pcm: np.ndarray, lang: str) -> Iterator[AsrResult]:
        for chunk in whisper.stream(denoised_pcm):
            yield AsrResult(text=chunk.text, is_final=chunk.is_final)
```

### 8.3 tts_engine（语音合成）

| 维度 | 规格 |
|---|---|
| 资源 | GPU 重（每实例 4 核 + 1×T4） |
| 延迟 | 300-800 ms 首字 |
| 模型 | Volcano / CosyVoice / Edge |
| 输出 | AudioChunk（流式 20ms G.711A） |

接口：
```python
class TtsEngine:
    def synthesize(self, text: str, voice_type: str) -> Iterator[AudioChunk]:
        for chunk in volcano.stream_synth(text, voice_type):
            yield AudioChunk(audio_data=chunk.bytes, seq=chunk.seq)
```

---

## 9. 资源预估

### 9.1 单实例（中等吞吐）

| 子模块 | CPU | GPU | 内存 |
|---|---|---|---|
| vad_denoise | 1-2 核 | - | 200 MB |
| asr_engine | 8 核 | 1× A10 | 16 GB |
| tts_engine | 4 核 | 1× T4 | 8 GB |
| PipelineOrchestrator | 0.5 核 | - | 100 MB |
| GatewayBridge + TalkService (gRPC) | 1 核 | - | 500 MB |
| **总计** | **12-15 核** | **1× A10 + 1× T4** | **25 GB** |

或 GPU 资源不够时：降级到 1× A10（同时跑 asr + tts，吞吐减半）。

### 9.2 K8s 部署

```yaml
spec:
  replicas: 5
  nodeSelector: { workload: gpu }
  containers:
    - name: media-service
      image: media-service:0.1.0
      resources:
        requests: { cpu: "8", memory: "16Gi", "nvidia.com/gpu": 1 }
        limits:   { cpu: "16", memory: "32Gi", "nvidia.com/gpu": 1 }
      env:
        - { name: VAD_BACKEND,  value: "silero" }
        - { name: DENOISE_BACKEND, value: "rnnoise" }
        - { name: ASR_BACKEND,  value: "whisper" }
        - { name: TTS_BACKEND,  value: "volcano" }
        - { name: AITALK_GRPC,  value: "aitalk-grpc:50051" }
```

---

## 10. 扩缩容策略

| 触发 | 行为 |
|---|---|
| `asr_queue_depth > 10` 持续 1 分钟 | +1 实例 |
| `asr_latency_ms_p95 > 1500ms` | +2 实例 |
| `vad_denoise_queue_depth > 100` | +1 实例 |
| `tts_first_chunk_ms_p95 > 800ms` | +2 实例 |
| `active_streams < 5` 持续 5 分钟 | -1 实例 |

---

## 11. 错误处理

| 失败 | 降级 |
|---|---|
| vad_denoise 不可用 | 跳过降噪直接发 ASR |
| asr_engine 不可用 | 返回 `AsrError("ASR_DOWN")`，AITalk text-only |
| tts_engine 不可用 | 返回 `TtsError("TTS_DOWN")`，AITalk text-only |
| 输入音频异常 | 返 `AsrError("INVALID_INPUT")` |
| GPU OOM | 重启实例，LB 切走 |

---

## 12. MediaService 子模块拆分的演进路径

| 阶段 | 描述 |
|---|---|
| Phase A | 三子模块同进程（默认，简单） |
| Phase B | vad_denoise 拆出（流量大时） |
| Phase C | asr_engine 拆出（GPU 资源吃紧时） |
| Phase D | tts_engine 拆出（TTS 高峰时段独立扩缩） |

**触发拆分条件**：单实例 GPU 利用率持续 > 80% 时，把对应的子模块拆出。

---

## 13. 与其他服务的关系

| 关系 | 说明 |
|---|---|
| Gateway | gRPC GatewayBridge，接收设备消息 + 音频，返回 AITalk 转发的文字 + 合成音频 |
| AITalk | gRPC TalkService，发送 ASR 文字 + 控制消息，接收文字 + TTS 文字 |
| 外部 ASR / TTS | HTTPS / WSS，被 MediaService 内部子模块调用 |
| PostgreSQL / Redis | MediaService 不直接用，所有配置和状态都在 AITalk |

---

## 14. 总结

MediaService 是 **音频 AI 一条龙**：

- 接收：设备的音频字节 + 控制消息
- 处理：denoise → vad → asr（上行）；tts（下行）
- 输出：AITalk 文字事件 + 设备音频字节

**关键设计点**：
- 上游（Gateway）和下游（AITalk）解耦
- 内部三子模块默认合一，必要时独立拆分
- 音频字节不出 MediaService 边界 → 网络最优
- 资源：CPU + GPU 混合，弹性扩缩容

放在 Gateway 和 AITalk 之间，自然形成"协议 → 音频 AI → 业务"的三层管道，**每层只关心自己那一段**，互不耦合。