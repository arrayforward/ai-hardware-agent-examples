# 云端业务设计方案

> 输入：`docs/cloud_gateway/settings_app_logic.md`（设备端 settings app 完整逻辑梳理）  
> 目标：基于设备端的配置 + 对话需求，反推云端需要实现的业务能力

---

## 1. 设计原则

| 原则 | 说明 |
|---|---|
| **配置与对话分离** | 配置是 1 次性 / 偶发性；对话是高 QPS 长连接 |
| **设备被动、云端主动** | 设备发起 hello，云端决定后续所有推送节奏 |
| **状态可恢复** | 设备掉线重连后，云端按 `device_name + session_token` 恢复上次会话 |
| **协议兼容设备端** | 字段命名、JSON 结构、message type 命名沿用 `convai_*` 体系 |
| **可插拔后端** | ASR / LLM / TTS / Function Calling Router 都是接口 + 默认实现 |

---

## 2. 业务能力清单（按 settings app 行为反推）

### 2.1 设备注册与凭证校验

| 触发 | 设备端调用 | 云端要做的 |
|---|---|---|
| 首次开机 | `convai_create(info_json, ...)` | 校验 `product_id + product_key + product_secret` 三元组 |
| 凭证错误 | （桥接层返回错误） | 返回 401 / 短期封禁该 device_name |
| 设备名变更 | hello body 携带新 `device_name` | 允许改名（同一 product 内），记录历史 |

**REST**：`POST /v1/device/auth`

```json
// request
{ "product_id":"...", "product_key":"...", "product_secret":"...", "device_name":"..." }

// response (success)
{ "session_token":"jwt...", "expires_at":1737034800, "device_id":"d_xxx" }

// response (failure)
{ "error":"AUTH_FAILED", "message":"invalid product_key" }
```

### 2.2 配置下发（5 维设置）

#### A. 音色目录查询
设备首次连接时，云端要把可用音色列表发给设备：

**REST**：`GET /v1/config/voices?gender=female`

```json
{
  "voices": [
    { "id":"Chinese (Mandarin)_Warm_Girl",      "label":"温柔少女",  "gender":"female", "preview_url":"..." },
    { "id":"Chinese (Mandarin)_BashfulGirl",    "label":"害羞女孩",  "gender":"female", "preview_url":"..." },
    ...
  ]
}
```

#### B. 性格 / 关系目录查询
**REST**：`GET /v1/config/personalities`、`GET /v1/config/relationships`

每个目录项的结构：
```json
{
  "id": "personality_gentle",
  "label": "温柔治愈",
  "prompt": "你是一个温柔又治愈的小伙伴……"
}
```

#### C. 实时配置变更（settings 的核心流程）
设备发出：
```json
{
  "type":"config_update",
  "seq": 42,
  "ts": 1737034812000,
  "body": {
    "config": {
      "llm_config": { "system_messages": ["base","personality","relationship"] },
      "tts_config": { "provider_params": { "audio": { "voice_type": "..." } } }
    }
  }
}
```

云端要做的：
1. 校验 `system_messages[0]` 是否等于"base_prompt"（防设备篡改）
2. 校验 `voice_type` 是否在目录内
3. **重新拼装 system prompt**（因为云端可能需要把 personality + relationship 的 ID 翻译成完整 prompt 后再注入）
4. ACK 给设备

响应：
```json
// success
{ "type":"config_update_ack", "seq":43, "body":{ "result":"ok", "applied_at":1737034812123 } }

// failure
{ "type":"config_update_err", "seq":43, "body":{ "code":"INVALID_VOICE", "message":"voice_type X not in catalogue" } }
```

**回滚语义**：如果云端返回 `config_update_err`，设备应回滚 UI（settings 已经实现此逻辑）。

#### D. 持久化
云端必须把 `device_id → 当前 (avatar, voice_id, personality_id, relationship_id, api_key)` 持久化在数据库，下次 hello 时自动恢复。

### 2.3 实时音频流

#### A. 上行（设备 mic → 云端 ASR）
- 设备每 20ms 发 160 字节 G.711A + 13 字节头（参见 `gateway/protocol.md`）
- 云端 ASR 完成后，触发 LLM 推理

#### B. 下行（云端 TTS → 设备 speaker）
- 流式发送 Start → Frames ×N → End
- 帧大小建议 ≤ 4 KB（设备 ring buffer 8 KB）

#### C. VAD（Voice Activity Detection）
**两种策略**：
- **服务端 VAD**（推荐）：云端检测音频能量/VAD，自行决定何时结束一段话、触发 ASR → LLM
- **客户端 VAD**：设备发送 `AudioOp::End` 显式标记

**生产推荐服务端 VAD**，降低设备功耗并减少上行流量。

### 2.4 Function Calling（emotion 等）

云端 LLM 通过 tool calling 协议驱动设备行为。

**已知工具**（settings 已实现）：
| name | 参数 | 设备侧效果 | 实现位置 |
|---|---|---|---|
| `emotion` | `{"emotion":"neutral|happy|angry|sad|doubt"}` | 切表情帧序列 | settings `talk_current_emotion` |

**可扩展工具**（云端 schema）：
| name | 参数 | 设备侧效果 | 优先级 |
|---|---|---|---|
| `play_sound` | `{"url":"https://...","volume":0.8}` | 播音效 | 中 |
| `set_volume` | `{"level":0..100,"stream":"system\|tts\|alarm"}` | 音量调节 | 中 |
| `show_emoji` | `{"id":"heart\|star\|fire","x":100,"y":50,"duration_ms":2000}` | 屏幕显示表情 | 低 |
| `navigate_to` | `{"app":"AItalk\|Story\|Recorder\|..."}` | 切换 app | 低 |
| `record_video` | `{"duration_sec":10,"resolution":"720p"}` | 录视频 | 后期 |

**协议**：完全沿用 OpenAI Realtime API 的 function calling 格式。

### 2.5 状态推送（6 种 + 4 种事件）

| 设备侧用途 | 触发时机 | 频率 |
|---|---|---|
| 状态灯 "● 已连接" / "● 未连接" | CONNECTED / DISCONNECTED | 一次 |
| "倾听/思考/回答" 状态文字 | 6 种状态任意切换 | 数十次/回合 |
| 表情帧切换 | emotion function_call | 1~3 次/回合 |
| "AI 服务已启动/已更新" | config_update_ack | 偶发 |

云端必须保证这 5 个通道（status × 6 + event × 4 + audio + text + fc）按 SDK 的 `on_*` 回调签名对齐。

---

## 3. 业务数据模型

### 3.1 实体

```sql
-- 设备主表
devices (
    id              BIGINT PK,
    product_id      VARCHAR(64),
    device_name     VARCHAR(64),
    device_secret   VARCHAR(128),       -- bcrypt
    created_at      TIMESTAMP,
    last_seen_at    TIMESTAMP
)

-- 设备当前配置
device_configs (
    device_id       BIGINT PK FK,
    avatar          VARCHAR(8),         -- 'female' | 'male'
    voice_id        VARCHAR(64),
    personality_id  VARCHAR(32),
    relationship_id VARCHAR(32),
    api_key         VARCHAR(128),
    updated_at      TIMESTAMP
)

-- 音色目录
voices (
    id              VARCHAR(64) PK,      -- "Chinese (Mandarin)_Warm_Girl"
    label           VARCHAR(32),         -- "温柔少女"
    gender          VARCHAR(8),
    sample_url      VARCHAR(256),
    enabled         BOOLEAN
)

-- 性格 prompt
personalities (
    id              VARCHAR(32) PK,      -- "personality_gentle"
    label           VARCHAR(32),         -- "温柔治愈"
    prompt          TEXT
)

-- 关系 prompt
relationships (
    id              VARCHAR(32) PK,
    label           VARCHAR(32),
    gender          VARCHAR(8),          -- 'female' | 'male' | 'any'
    prompt          TEXT
)

-- 会话日志（可选，用于对话质量分析）
sessions (
    id              BIGINT PK,
    device_id       BIGINT FK,
    started_at      TIMESTAMP,
    ended_at        TIMESTAMP,
    asr_text        TEXT,
    llm_reply       TEXT,
    tool_calls      JSONB,              -- [{"name":"emotion","args":{...}}]
    audio_bytes_in  BIGINT,
    audio_bytes_out BIGINT
)
```

### 3.2 配置拼装算法（云端）

设备发来：
```json
"system_messages": ["base_prompt", "<personality_text>", "<relationship_text>"]
```

云端要做 **冗余校验 + 翻译**：
1. 校验 `system_messages[0] == "base_prompt"`（防设备伪造）
2. **可选**：把 `system_messages[1]` / `[2]` 替换为数据库里的完整 prompt（即使设备发的就是完整文本）
3. 把 `tts_config.provider_params.audio.voice_type` 映射到实际 TTS provider 的 voice ID

**注意**：当前 settings 直接在设备端拼好文本发送，云端默认信任。如果要严防篡改，云端必须自己重新组装 prompt，而不是使用设备发来的字符串。

---

## 4. 对话业务流（完整时序）

```
设备                              云端                                    后端
 │                                  │                                       │
 │──[HELLO + info + token]─────────▶│                                       │
 │                                  │──[verify device + load config]──────▶│ MySQL
 │                                  │                                       │
 │◀─[hello_ack + audio_config]────│                                       │
 │◀─[event connected]──────────────│                                       │
 │◀─[status listening]─────────────│                                       │
 │                                  │                                       │
 │──[BIN 0x11 Start]───────────────▶│ VAD begin                             │
 │──[BIN 0x10 Frame × 50]─────────▶│                                       │
 │──[BIN 0x12 End]─────────────────▶│ VAD end                               │
 │                                  │──[ASR: 1000 帧 16kHz]───────────────▶│ Whisper
 │                                  │◀─[text:"今天我们学画画"]────────────│
 │◀─[status thinking]──────────────│                                       │
 │                                  │──[LLM chat]─────────────────────────▶│ GPT-4o
 │                                  │   tools=[emotion]                     │
 │                                  │◀─[stream chunks:                      │
 │                                  │   {"text":"好呀！"}                   │
 │                                  │   {"fc":"emotion=happy"}]─────────────│
 │◀─[status answering]─────────────│                                       │
 │◀─[text "好呀！"]────────────────│                                       │
 │◀─[function_call emotion=happy]─│                                       │
 │──[function_call_output ACK]────▶│──[feed tool result back to LLM]──────▶│ GPT-4o
 │                                  │◀─[text:"先画个太阳"]                  │
 │                                  │──[TTS synth: "好呀！先画个太阳"]────▶│ Volcano TTS
 │◀─[BIN 0x11 Start]──────────────│                                       │
 │◀─[BIN 0x10 Frame × 32]─────────│                                       │
 │◀─[BIN 0x12 End]────────────────│                                       │
 │◀─[status answer_finished]──────│                                       │
 │◀─[status listening]─────────────│  等待下一轮                            │
```

---

## 5. 配置变更业务流

### 5.1 切换性格（settings 的 on_ai_setting_changed）

```
设备                                   云端
 │──[config_update: personality=古灵精怪]▶│
 │                                  │──[校验: 性格 ID 有效]──────────────────▶│ DB
 │                                  │──[查 personality.prompt]────────────────▶│
 │                                  │──[重组 system_messages]                  │
 │                                  │──[通知 LLM provider 切换 system prompt]│
 │◀─[config_update_ack ok]────────│
 │                                  │──[持久化 device_configs]────────────────▶│ DB
 │◀─[event updated]────────────────│
```

**关键**：云端必须**异步通知下游**（如 LLM session 缓存、监控指标），不能只更新数据库。

### 5.2 APIKey 用户级（settings 仅 UI，未下发）

**未来扩展**：当设备端补全 APIKey 下发时：

```
设备 ─[config_update: api_key="user_xxx"]─▶ 云端
                                          ├─ 加密存储（per-device-key）
                                          ├─ 用于跨设备共享数据 / 第三方服务调用
                                          └─ 失效时返回 config_update_err(code=INVALID_KEY)
```

**风险**：APIKey 是用户隐私，不能明文存储。建议用 `device_pub_key` 加密后存，设备每次查询需解密。

---

## 6. 错误码体系（云端 → 设备）

| HTTP 状态 | code | 含义 | 设备处理 |
|---|---|---|---|
| 400 | `INVALID_CONFIG` | config_update 字段不合法 | UI 回滚 + Toast |
| 401 | `AUTH_FAILED` | 凭证错误 | 重启 SDK init（清缓存） |
| 403 | `NO_PERMISSION` | 该 product 未开通 AI 服务 | 同 401 |
| 404 | `DEVICE_NOT_FOUND` | device_name 未注册 | 同 401 |
| 409 | `CONFLICT` | device_name 被占用 | 提示改名 |
| 429 | `RATE_LIMITED` | 调用过频 | UI 提示稍后再试 |
| 500 | `INTERNAL` | 云端故障 | 重试 3 次 |
| 503 | `UPSTREAM_DOWN` | ASR/LLM/TTS 服务不可用 | 提示"服务暂不可用" |

**实现要求**：所有错误必须含 `code` 字段供设备精确处理，不能只靠 HTTP 状态。

---

## 7. 配额与限流

| 维度 | 限制 | 超出行为 |
|---|---|---|
| 单设备并发连接 | 1 | 第二次 hello 返 `ALREADY_CONNECTED`，关闭旧连接 |
| 单设备上行带宽 | 32 KB/s | 超出降采样到 8 KB/s |
| 单设备音频时长 | 60 分钟/天 | `RATE_LIMITED`，次日 0 点重置 |
| LLM token | 100k tokens/天 | `RATE_LIMITED` |
| TTS 字符 | 10k 字/天 | `RATE_LIMITED` |
| Function call | 50 次/分钟 | 排队，超过 10 个返错 |

---

## 8. 监控指标（云端必须暴露）

| 指标 | 类型 | 维度 |
|---|---|---|
| `active_sessions` | Gauge | product_id |
| `asr_latency_ms` | Histogram | product_id |
| `llm_first_token_ms` | Histogram | product_id |
| `tts_total_ms` | Histogram | voice_id |
| `audio_bytes_in_per_sec` | Gauge | device_id |
| `audio_bytes_out_per_sec` | Gauge | device_id |
| `function_call_total` | Counter | name |
| `error_total` | Counter | code |

Prometheus 端点：`GET /metrics`

---

## 9. 安全要点

1. **TLS 终结**：生产必须 nginx / Envoy 终结 TLS（参见 deployment.md）
2. **token 过期**：session_token 24h 过期，过期前自动续签
3. **设备白名单**：每个 product 维护允许的 device_name 前缀
4. **凭证加密**：product_secret 在数据库中 bcrypt 存储
5. **音频日志**：默认不持久化音频流；如需录制需明确 opt-in
6. **PII 处理**：ASR 文本自动脱敏后再入库

---

## 10. 业务演进路线图

| 阶段 | 目标 | 新增能力 |
|---|---|---|
| v1.0（现）| 5 维配置 + emotion FC | 单设备 1 路会话 |
| v1.1 | 多设备同步 + 历史对话 | 跨设备会话接力、对话日志 |
| v1.2 | 第三方插件 | `play_sound`、`navigate_to` FC |
| v1.3 | 多模态 | 视频输入、图像理解 |
| v2.0 | AI Agent 框架 | 用户自定义 tool schema、动态 personality |

---

## 11. 总结：云端业务的核心 KPI

| 指标 | 目标 |
|---|---|
| hello → connected 延迟 | < 500 ms |
| ASR 端到端延迟（VAD 结束 → ASR 文本）| < 800 ms |
| LLM 首 token 延迟 | < 500 ms |
| TTS 总延迟（合成 30 字）| < 600 ms |
| emotion function_call 延迟 | < 200 ms |
| 单会话 1 小时 音频上行带宽 | < 5 MB |
| 单网关支持并发会话 | > 100 |

云端的实现核心是 **网关（cloud_gateway）+ ASR/LLM/TTS 编排器 + 配置中心 + 监控**四件套。本文档已经把它们之间的接口形态和数据流向全部锁定。

---

## 12. 与 cloud_gateway 网关的对应关系

| 本文档业务能力 | cloud_gateway 实现位置 |
|---|---|
| § 2.1 设备鉴权 | 现有 stub 直接通过，需要扩展 `Session::on_hello` 调实际 auth REST |
| § 2.2 配置下发 | 现有 `config_update` 走通，需要扩展为读 DB + 翻译 |
| § 2.3 音频流 | 现有 ASR/LLM/TTS stub 验证 pipeline，需要替换为真实后端 |
| § 2.4 Function Calling | 现有 `cloud_message_callback` 已实现 emotion，需要新增 tool 注册表 |
| § 2.5 状态推送 | 已实现 6 状态 + 4 事件 |
| § 6 错误码 | 需新增 `on_convai_event` FAILED 分支的错误 message 格式 |
| § 7 限流 | 需在 Session 增加 token bucket |
| § 8 监控 | 需新增 `/metrics` HTTP 端口 |

---

> 设备端 settings app 是 **AI 硬件的"控制面板"**；云端业务则是 **这个控制面板的远端总台**。两者通过本协议（WebSocket + JSON + 二进制音频）联动，构成完整的 AI 硬件体验闭环。