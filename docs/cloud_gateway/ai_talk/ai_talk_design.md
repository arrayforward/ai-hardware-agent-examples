# AITalk 服务设计

> 服务代号：`ai-talk`  
> 位置：**MediaService 之后**（最下游）  
> 上下游：上接 MediaService（仅文字事件），下接外部 LLM  
> 资源特征：**业务编排 + 状态管理**，无 GPU，CPU 即可

---

## 1. 单一职责

> **AITalk 是整个云端业务的"大脑"——拿到用户说了什么之后，决定要回复什么。**

具体：
- 接收 ASR 文字 → 拼装 LLM prompt → 调 LLM → 流式返回文字 + function calls
- Function Calling 路由（决定调用本机工具 or 透传给设备）
- 5 维配置管理（avatar / voice / personality / relationship / api_key）
- 会话状态机（6 种状态）
- 对话历史 + 配置持久化
- 配额 / 计费 / 监控

## 2. 不做的事

- ❌ 设备协议 → 交给 Gateway
- ❌ 音频处理（denoise / vad / asr / tts）→ 交给 MediaService
- ❌ 音频字节解析（AudioService 完成）

---

## 3. 位置与上下游

```
设备 → Gateway (WS) → MediaService (音频 AI) → AITalk (业务) → LLM
                                                       │
                                                       └─ 调外部 AI
```

**关键**：AITalk 只与 MediaService 交互，**不直接接触 Gateway 或设备**。

---

## 4. 架构

```
   MediaService                       AITalk                         外部
       │                                │                            │
       │─gRPC stream (TalkService)─────▶│                            │
       │  AsrFinal / VadEvent           │                            │
       │  ConfigUpdate / FCOutput / ... │                            │
       │                                │─[LLM stream]───────────────▶│GPT-4o
       │                                │                            │
       │◀─gRPC stream─────────────────│                            │
       │  TextDelta / FunctionCall       │                            │
       │  TtsText / Status / Event       │                            │
       │                                │─[PG: load device config]───▶│PostgreSQL
       │                                │─[Redis: session state]────▶│Redis
```

---

## 5. 核心数据模型

### 5.1 PostgreSQL（持久化）

```sql
-- 设备
CREATE TABLE devices (
    id              BIGSERIAL PRIMARY KEY,
    product_id      VARCHAR(64)  NOT NULL,
    device_name     VARCHAR(64)  NOT NULL,
    device_secret   VARCHAR(128) NOT NULL,    -- bcrypt
    status          VARCHAR(16)  DEFAULT 'active',
    created_at      TIMESTAMPTZ  DEFAULT now(),
    last_seen_at    TIMESTAMPTZ
);
CREATE UNIQUE INDEX idx_devices_pn ON devices(product_id, device_name);

-- 5 维配置
CREATE TABLE device_configs (
    device_id       BIGINT PRIMARY KEY REFERENCES devices(id),
    avatar          VARCHAR(8)   DEFAULT 'female',
    voice_type      VARCHAR(64)  DEFAULT 'Chinese (Mandarin)_Warm_Girl',
    personality_id  VARCHAR(32)  DEFAULT 'personality_gentle',
    relationship_id VARCHAR(32)  DEFAULT 'relationship_kind_sister',
    api_key         VARCHAR(128),
    system_overlay  JSONB,
    updated_at      TIMESTAMPTZ  DEFAULT now()
);

-- 音色目录
CREATE TABLE voice_catalog (
    id              VARCHAR(64) PRIMARY KEY,
    provider_id     VARCHAR(32) NOT NULL,      -- "volcano" / "cosyvoice"
    provider_voice  VARCHAR(64) NOT NULL,
    label           VARCHAR(32) NOT NULL,
    gender          VARCHAR(8)  NOT NULL,
    language        VARCHAR(16) NOT NULL,
    sample_url      VARCHAR(256),
    enabled         BOOLEAN      DEFAULT true
);
CREATE INDEX idx_voice_gender ON voice_catalog(gender, enabled);

-- 性格目录
CREATE TABLE personality_catalog (
    id          VARCHAR(32) PRIMARY KEY,
    label       VARCHAR(32) NOT NULL,
    prompt      TEXT NOT NULL
);

-- 关系目录
CREATE TABLE relationship_catalog (
    id          VARCHAR(32) PRIMARY KEY,
    label       VARCHAR(32) NOT NULL,
    gender      VARCHAR(8),
    prompt      TEXT NOT NULL
);

-- 会话日志
CREATE TABLE sessions (
    id              BIGSERIAL PRIMARY KEY,
    device_id       BIGINT REFERENCES devices(id),
    session_uuid    UUID UNIQUE NOT NULL,
    started_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    ended_at        TIMESTAMPTZ,
    turn_count      INT DEFAULT 0,
    asr_chars       INT DEFAULT 0,
    llm_tokens_in   INT DEFAULT 0,
    llm_tokens_out  INT DEFAULT 0,
    tts_chars       INT DEFAULT 0,
    tool_calls      JSONB DEFAULT '[]'::jsonb
);
CREATE INDEX idx_sessions_device ON sessions(device_id, started_at DESC);
```

### 5.2 Redis（短期状态）

| Key | TTL | 用途 |
|---|---|---|
| `session:{uuid}:state` | 60min | 会话状态机（IDLE/LISTENING/...） |
| `session:{uuid}:history` | 24h | LLM 上下文（短期记忆） |
| `session:{uuid}:llm` | 24h | LLM session ID（多轮） |
| `device:{id}:qos` | 60s | 限流滑动窗口 |
| `device:{id}:session_uuid` | 5min | 当前活跃 session，用于重复连接检测 |

---

## 6. 状态机

```
                     ┌──────────┐
                     │ INIT     │  (内存中创建，未持久化)
                     └────┬─────┘
                          │ hello_ack 成功
                          ▼
              ┌──────────────────────┐
              │ IDLE (Listening)      │ 等待 ASR / VadEvent
              └────┬─────────────────┘
                   │ VadEvent.SPEECH_START
                   ▼
              ┌──────────────────────┐
              │ LISTENING            │ ASR 进行中
              └────┬─────────────────┘
                   │ AsrFinal
                   ▼
              ┌──────────────────────┐
              │ THINKING             │ LLM 推理中
              └────┬─────────────────┘
                   │ LLM first token
                   ▼
              ┌──────────────────────┐
              │ ANSWERING            │ 流式返回 text + 触发 TTS
              └────┬─────────────────┘
                   │ 用户打断
                   ▼
              ┌──────────────────────┐
              │ INTERRUPTED          │ 取消 TTS 流
              └────┬─────────────────┘
                   │ LLM 流结束 + TTS 流结束
                   ▼
              ┌──────────────────────┐
              │ ANSWER_FINISHED       │ 短态
              └────┬─────────────────┘
                   │ 自动
                   ▼
              ┌──────────────────────┐
              │ IDLE (Listening)      │
              └──────────────────────┘
```

---

## 7. 接口契约（gRPC，对 MediaService 暴露）

```protobuf
syntax = "proto3";
package aitalk.v1;

service AuthService {
  // MediaService 转发 hello 时调
  rpc Authenticate(AuthRequest) returns (AuthResponse);
}

service TalkService {
  // MediaService → AITalk：文字 + 控制消息
  rpc Connect(stream MediaToAi) returns (stream AiToMedia);
}

message AuthRequest {
  string product_id     = 1;
  string product_key    = 2;
  string product_secret = 3;
  string device_name    = 4;
  string client_version = 5;
}

message AuthResponse {
  bool   ok             = 1;
  string session_token  = 2;
  int64  expires_at     = 3;
  string device_id      = 4;
  string error_code     = 5;
  string error_message  = 6;
}

message MediaToAi {     // MediaService → AITalk
  oneof body {
    AsrFinal           asr_final           = 1;
    AsrPartial         asr_partial         = 2;
    VadEvent           vad_event           = 3;
    ConfigUpdate       config_update       = 4;
    FunctionCallOutput function_call_output = 5;
    Bye                bye                 = 6;
    Ping               ping                = 7;
  }
  string session_id = 100;
  uint32 seq         = 101;
  uint64 ts_ms       = 102;
}

message AiToMedia {     // AITalk → MediaService
  oneof body {
    Status           status            = 1;
    Event            event             = 2;
    TextDelta        text_delta        = 3;
    FunctionCall     function_call     = 4;
    TtsText          tts_text          = 5;
    ConfigUpdateAck  config_update_ack = 6;
    ConfigUpdateErr  config_update_err = 7;
    Pong             pong              = 8;
  }
  string session_id = 100;
  uint32 seq         = 101;
  uint64 ts_ms       = 102;
}
```

**关键**：与 MediaService 之间**没有任何音频字节**——只有文字 + 控制消息。

---

## 8. 配置变更业务流（关键路径）

设备发来 config_update，**AITalk 必须重新拼装 system prompt**，不能直接信任设备：

```
[MediaService → AITalk.ConfigService.Apply]

1. 解析 body.config
2. 校验字段：
   - voice_type 在 voice_catalog.enabled 列表
   - personality_id 在 personality_catalog 列表
   - relationship_id 在 relationship_catalog 列表
   - 性别一致性（avatar=gender 与 relationship.gender 必须匹配）
3. 防篡改：忽略设备/MediaService 发的 system_messages[1]/[2]（如果是文本），
   改从 catalog 取完整 prompt
4. 拼装 LLM 配置：
   llm_config.system_messages = [
       base_prompt,                    // 来自 catalog
       personality.prompt,              // 来自 catalog
       relationship.prompt,             // 来自 catalog
       user_overlay                     // device_configs.system_overlay（可选）
   ]
   tts_config.provider_params.audio.voice_type = voice_catalog.provider_voice
5. 写 PostgreSQL device_configs
6. 更新 Redis session.{uuid}.llm 配置（让 LlmGateway 切换 system prompt）
7. 触发事件：device_configs.updated → 监控
8. 返回 ack 或 err（经 MediaService → Gateway → 设备）
```

---

## 9. 对话业务流

### 9.1 ASR 文字到达

```
[MediaService → AITalk.AsrFinal("今天我们学画画")]
   ├─ 状态机：IDLE → THINKING（VAD START 已触发 LISTENING）
   ├─ 发 Status(THINKING) → MediaService → Gateway → 设备
   ├─ 关闭 LLM 流前的清理
   └─ 喂给 LLM 流
```

### 9.2 LLM 流式返回

```
[LLM → AITalk.LlmGateway.Chunk(text_delta="好呀")]
   ├─ 状态机：THINKING → ANSWERING
   ├─ 发 TextDelta("好呀") → MediaService → Gateway → 设备
   └─ 同时把 TtsText("好呀") 提交给 MediaService 合成

[LLM → AITalk.LlmGateway.Chunk(tool_call=emotion(happy))]
   ├─ ToolRouter 路由：emotion → 透传给设备
   ├─ 发 FunctionCall(emotion=happy) → MediaService → Gateway → 设备
   └─ 等待设备的 function_call_output 应答（经 MediaService 中转）
       ├─ 收到 → 喂给 LLM 继续
       └─ 超时 5s → 注入 mock output "ok" 让 LLM 继续

[LLM 流结束]
   ├─ 发 Status(ANSWER_FINISHED) → 设备
   ├─ 写 PostgreSQL sessions（asr_chars, llm_tokens, tts_chars, tool_calls）
   └─ 状态机 → IDLE
```

### 9.3 TTS 触发

```
[LLM 第一个 text_delta 到达]
   ├─ AITalk 决定是否 TTS
   └─ 发 TtsText("好呀") → MediaService.Synthesize
        ↓ 流式
      MediaService → AITalk.TtsAudioChunk (G.711A bytes)
                       ├─ 发 AudioChunk → MediaService → Gateway → 设备
```

---

## 10. Function Calling 路由表

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

**schema 注册**（启动时加载）：
```python
TOOLS = [
    {
        "name": "emotion",
        "description": "设置 AI 表情",
        "parameters": {
            "type": "object",
            "properties": {
                "emotion": {"type": "string", "enum": ["neutral","happy","angry","sad","doubt"]}
            },
            "required": ["emotion"]
        },
        "executor": "device",   # 经 MediaService 透传
    },
    {
        "name": "web_search",
        "description": "搜索互联网",
        "parameters": {...},
        "executor": "cloud"
    },
]
```

---

## 11. 内部模块（同一进程内，但有清晰边界）

| 模块 | 职责 |
|---|---|
| SessionStore | 维护 session_id → Session 的内存表 |
| Session | 单设备的对话状态机、上下文、配置缓存 |
| LlmGateway | 调用 LLM（OpenAI / Qwen / 文心 等），流式返回 |
| ToolRouter | 解析 function_call，决定调用本机工具 or 透传给设备 |
| ConfigService | 5 维配置读 / 写 / 翻译（device ↔ 数据库） |
| HistoryService | 会话历史、摘要（短期存 Redis，长期写 PG） |
| Metering | token 计数、配额检查、计费 |

---

## 12. 横向扩展

### 12.1 状态外置

有状态（AITalk 持有 Session）通过 Redis 外置：
- 会话状态 → `session:{uuid}:state` (Redis)
- LLM 上下文 → `session:{uuid}:history` (Redis, 24h TTL)
- 持久配置 → PostgreSQL

**效果**：AITalk 实例可以无状态化，session_id 取模路由即可。

### 12.2 K8s 部署

```yaml
apiVersion: apps/v1
kind: Deployment
metadata: { name: ai-talk }
spec:
  replicas: 8
  template:
    spec:
      containers:
        - name: talk
          image: ai-talk:0.1.0
          env:
            - { name: PG_DSN,        valueFrom: { secretKeyRef: { name: pg, key: dsn } } }
            - { name: REDIS_URL,     value: "redis://redis:6379" }
            - { name: LLM_API_KEY,    valueFrom: { secretKeyRef: { name: openai, key: key } } }
          resources:
            requests: { cpu: "2", memory: "4Gi" }
            limits:   { cpu: "8", memory: "16Gi" }
```

**session affinity**：envoy 用 `ring_hash` 按 session_id 哈希路由（同一 session 总是到同一实例，减少 Redis 读取）。

---

## 13. 资源预估

| 并发 session | CPU | 内存 | GPU | 备注 |
|---|---|---|---|---|
| 100 | 4 核 | 8 GB | 0 | 单实例足够 |
| 1000 | 16 核 | 32 GB | 0 | 需 4-8 实例 |
| 10000 | 64 核 | 128 GB | 0 | 需 20+ 实例 + 分片 |

LLM 调用密集，主要瓶颈是 LLM API 的 RPS 配额。

---

## 14. 监控指标

| 指标 | 类型 |
|---|---|
| `aitalk_active_sessions` | Gauge |
| `aitalk_llm_latency_ms_p95` | Histogram |
| `aitalk_llm_first_token_ms_p95` | Histogram |
| `aitalk_tool_call_total` | Counter（label: name, executor） |
| `aitalk_config_apply_total` | Counter（label: result, field） |
| `aitalk_session_duration_seconds` | Histogram |
| `aitalk_tokens_total` | Counter（label: direction=in\|out） |

---

## 15. 关键设计决策

| 决策 | 选择 | 理由 |
|---|---|---|
| System prompt 拼装 | **服务端拼**（防篡改） | MediaService 可能被破解 |
| Function call 透传 | **经 MediaService 中转** | MediaService schema 演进不影响 AITalk |
| 会话状态存储 | **Redis + 内存双层** | Redis 兜底 |
| LLM 上下文窗口 | **最近 20 轮 + 摘要压缩** | 控制 token 成本 |
| Function call 超时 | **5s 没应答则 mock success** | 不阻塞 LLM |

---

## 16. 与 MediaService 的边界

| AITalk 看到的 | MediaService 看到但 AITalk 不知道的 |
|---|---|
| `AsrFinal.text`（字符串） | 原始 G.711A 字节 |
| `VadEvent.type`（枚举） | denoise 算法选择、模型版本 |
| `TtsText.text`（字符串） | tts 引擎选择、音色 ID 翻译 |
| `AudioConfig`（codec, sr） | 实际音频处理细节 |

**结果**：AITalk 完全不知道音频存在，只跟"文字"和"控制事件"打交道。

---

## 17. 总结

AITalk 是 **业务编排中枢**：

- 接收结构化输入（ASR 文字 / 配置变更）
- 编排 LLM + Function Call + TTS
- 输出结构化事件（状态 / 文字 / FC / TTS 请求）
- 持久化业务状态（配置、历史、配额）

位于 MediaService 之后，**不直接接触设备和音频字节**，是云端业务所有核心逻辑的所在地。