# Settings App 完整逻辑梳理

> 适用代码：`D:\ai-hardware-agent-examples\examples\goldieos\apps\settings\main_app.cpp`（1987 行）  
> 目的：彻底梳理设备端"AI 配置 + 对话入口"的所有业务流，作为云端业务设计的输入

---

## 1. 应用定位

`apps/settings` 不是单纯的"系统设置页"，而是一个 **AI 配置中心 + AI 对话演示入口**：

| 功能维度 | 是否本 app 实现 | 备注 |
|---|---|---|
| 形象选择（男/女头像） | ✅ | 决定默认音色/个性/关系 |
| TTS 音色选择（3 套方案） | ✅ | 编译宏切换方案 |
| AI 性格选择（5 种带 system prompt） | ✅ | 切换时实时下发 |
| AI 关系选择（10 种带 system prompt） | ✅ | 暖心大姐姐 / 暖心大哥哥 等 |
| 用户级 APIKey | ✅ | 仅 UI，**未实际下发** |
| AI 服务开关 | ✅ | start / stop / restart |
| **AI 实时对话演示页** | ✅ | 内嵌（含表情动画） |
| WiFi 配置 | ✅ | STA 模式 + 扫描 + 连接 |
| 星闪（NearLink）配置 | ✅ | 主机/从机模式 + 配对 |
| 音量设置 | ✅ | 进度条 + set_volume |

---

## 2. 状态机（5 个核心页面 + 3 个子页）

```
                   ┌────────────┐
                   │ 主页面     │  FrameView_0
                   │ (系统设置) │
                   └─────┬──────┘
         ┌──────────────┼──────────────┬──────────────┬─────────────┐
         ▼              ▼              ▼              ▼             ▼
   ┌──────────┐   ┌──────────┐   ┌──────────┐   ┌──────────┐  ┌──────────┐
   │ AI设置页 │   │ WiFi 页  │   │ 星闪页   │   │ 音量页   │  │ (其他)   │
   │ FrameView│   │ FrameView│   │ FrameView│   │ FrameView│  │          │
   │ _cloud   │   │ _wifi    │   │ _sle     │   │ _volume  │  │          │
   └────┬─────┘   └──────────┘   └──────────┘   └──────────┘  └──────────┘
        │                                                  ▲
        │ 进入对话（引擎已连接时）                          │
        ▼                                                  │
   ┌──────────┐                                             │
   │ 对话页   │  FrameView_talk  ← BACK 键回 AI 配置页       │
   │ (全屏)   │                                             │
   └──────────┘                                             │
        │ BACK                                              │
        └──────────────────────────────────────────────────┘
                                              BACK → 主页面
```

### 2.1 AI 设置页子页

```
   FrameView_cloud
        │
        ├─[头像/音色/性格/关系/APIKey] 按钮 → show_xxx_setting()
        │
        ▼
   FrameView_config_wm（弹窗式）
        │
        ├─ LabelView_cfgtitle0: "形象/音色/个性/关系 设置"
        ├─ ListView_cfgwmlist: 选项列表（5~10 项）
        ├─ TextEditView_apikey: 仅 APIKey 页显示
        └─ ButtonView_yes17 / ButtonView_cancle17: 仅 APIKey 页显示
```

按下列表项 → `on_ai_setting_changed()` → `apply_ai_settings()` → `convai_update()`  
按 BACK → `show_cloud_page()`（保留引擎）

---

## 3. 启动序列（`goldie_app_run`，main_app.cpp:1818）

```cpp
main_ui_init();                    // 初始化所有 view
init_views();                      // 注册 30+ 按钮/列表回调
sdk_engine = convai_bridge_get_engine();
convai_bridge_on_status(cloud_status_callback);    // 状态回调
convai_bridge_on_event(cloud_event_callback);      // 事件回调
convai_bridge_on_message(cloud_message_callback);  // JSON 消息回调
init_cloud_configs();             // 设默认配置

// 关键：开机就把默认配置 JSON 写入桥接层
char *json_buf = goldie_malloc(2048);
generate_convai_config_json(json_buf, 2048);
convai_bridge_set_startup_config(json_buf);   // 桥接层缓存

// 启动动画线程（待机，进入对话页时激活）
talk_init_flag = 1;
talk_thread = new Goldie_Thread(talk_play_task, NULL, 0x1000);

Window_main->flush(0, 0, APP_WINDOW_WIDTH, APP_WINDOW_HEIGHT);
```

---

## 4. 配置 JSON 生成（`generate_convai_config_json`，main_app.cpp:1311）

```cpp
const char *base_prompt = "你的名字叫小荷，你可以帮小朋友解决小烦恼哦。";
const char *personality_str = personality_prompt[current_personid];    // 5 选 1
const char *voice_str;
const char *relationship_str;

if (current_avatrid < 1 /* 男性阈值 */) {
    voice_str = voice_type_female[current_voiceid];
    relationship_str = relationship_prompt_female[current_relatid];
} else {
    voice_str = voice_type_male[current_voiceid];
    relationship_str = relationship_prompt_male[current_relatid];
}

snprintf(buf, 2048,
  "{"
    "\"config\":{"
      "\"llm_config\":{"
        "\"system_messages\":["
          "\"%s\","        // base_prompt
          "\"%s\","        // personality
          "\"%s\""         // relationship
        "]"
      "},"
      "\"tts_config\":{"
        "\"provider_params\":{"
          "\"audio\":{\"voice_type\":\"%s\"}"
        "}"
      "}"
    "}"
  "}",
  base_prompt, personality_str, relationship_str, voice_str);
```

### 4.1 音色映射表（3 套编译宏切换）

| 宏 | 女声 ID | 男声 ID |
|---|---|---|
| `CONVAI_USE_MINIMAX_VOICE`（默认） | `Chinese (Mandarin)_Warm_Girl` / `_BashfulGirl` / `_Warm_HeartedGirl` / `_Kind-hearted_Elder` | `Chinese (Mandarin)_Gentleman` / `_Humorous_Elder` / `_Stubborn_Friend` / `_Pure-hearted_Boy` / `_Cute_Spirit` / `Robot_Armor` |
| `CONVAI_USE_OLD_VOICE` | `CharmingGirlfriend` / `GentleSister` | `ElegantUncle` / `BeijingYoungMaster` / `SichuanBoy` |
| 默认（X4） | `X4_YEZI` / `AISJIUXU` | `X4_GUANSHAN` / `X4_DOUDOU` |

### 4.2 5 种 personality prompt（性格中文 system prompt）

```cpp
static const char* personality_prompt[] = {
    "你是一个温柔又治愈的小伙伴，说话像棉花糖一样软乎乎的……",   // 温柔治愈
    "你是一个古灵精怪的小伙伴，说话像跳跳糖一样噼里啪啦……",   // 古灵精怪
    "你是一个聪明又稳重的小伙伴……",                              // 聪明稳重
    "你是一个充满诗意的小伙伴……",                                // 充满诗意
    "你是一个阳光满格的小伙伴……",                                // 阳光满格
};
```

### 4.3 10 种 relationship prompt（关系中文 system prompt）

性别 × 关系 = 10 种 system prompt，例如：
- 女 + 暖心大姐姐："你现在扮演小朋友的暖心大姐姐，像亲姐姐一样温柔可靠……"
- 男 + 暖心大哥哥："你现在扮演小朋友的暖心大哥哥，像亲哥哥一样可靠又有担当……"
- 女 + 故事守护者："你现在扮演小朋友的故事守护者……睡前轻声讲述美好的故事……"

---

## 5. 配置变更流（关键回调链）

```
用户在 FrameView_config_wm 选中某项
   ↓
ListView_cfgwmlist::setOnItemClick
   ↓
on_ai_setting_changed(itemId, cloud_current_cfg_page)
   │  ├─ save_ai_config()             // 备份 4 个 ID
   │  ├─ 更新 current_avatrid / voiceid / personid / relatid
   │  │   （切性别时连带重置默认音色/个性/关系）
   │  └─ apply_ai_settings()
   ↓
apply_ai_settings()                     // main_app.cpp:1362
   ├─ generate_convai_config_json(buf, 2048)   // 生成完整 JSON
   ├─ convai_bridge_set_startup_config(json_buf)  // 桥接层缓存（总是执行）
   ├─ if (!sdk_engine) return;          // 引擎未起：仅保存
   └─ convai_update(sdk_engine, json_buf)
        ├─ 成功 → 桥接层 CONVAI_EV_UPDATED 事件 → 状态灯显示 "● 已连接"
        └─ 失败 → restore_ai_config() + 回滚 UI（头像图+名称）
```

**回滚机制**：失败后调用 `restore_ai_config()` 把 4 个 ID 恢复到 `backup_*` 值，然后刷新头像 LabelView_avashow0 和 LabelView_pic。

---

## 6. AI 实时对话页（这是 settings app 的核心创新）

### 6.1 UI 布局（main_ui.h:613-735）

```
┌─────────────────────────────┐
│  FrameView_talk_L  (0~160)   │
│   LabelView_talk_eyeL (88×85)│  闭/睁/半/怒/哀/疑 眼睛
├─────────────────────────────┤
│  FrameView_talk_R  (160~320) │
│   LabelView_talk_eyeR (88×85)│  右眼（与左眼异帧以做表情）
│   LabelView_talk_tie (56×53) │  男：领结  女：蝴蝶结
├─────────────────────────────┤
│  LabelView_talk_text (240×16)│  "聆听中..." / "思考中..." / "回答中..."
└─────────────────────────────┘
```

### 6.2 状态映射（`talk_play_task`，main_app.cpp:1231）

```cpp
while (talk_running_flag) {
    sdk_status = bridge_get_status();   // 6 种之一
    switch (sdk_status) {
        case IDLE           → PLAY_TYPE_SLEEP,  文字 "待机中...."
        case LISTENING      → PLAY_TYPE_SILENCE, "聆听中...."
        case THINKING       → PLAY_TYPE_SILENCE, "思考中...."
        case ANSWERING      → PLAY_TYPE_SPEAK,   "回答中...."
        case INTERRUPTED     → PLAY_TYPE_SILENCE, "已打断"
        case ANSWER_FINISHED→ PLAY_TYPE_SILENCE, "正在思考...."
    }
    update_talk_avatar_ui(talk_play_type);   // 200ms 一帧
    FrameView_talk->flush();
    goldie_msleep(200);
}
```

### 6.3 5 种情绪 × 男/女 = 10 套表情帧序列

| emotion | 男帧序列 | 女帧序列 |
|---|---|---|
| neutral | 睁眼 | 睁眼（new） |
| happy | `laugh_l/r_88_85` + y 抖动 ±10px | `laugh_l/r_new_88_85` + y 抖动 |
| angry | `angry_male_l_88_85` + `r1`/`r2` 交替 | `angry_female_l_88_85` + `r1`/`r2` |
| sad | `sad_male_l/r_88_85`，中间穿插闭眼 | `sad_female_l/r_88_85` |
| doubt | `doubt_male_l_88_85` + `r1`/`r2` | `doubt_female_l_88_85` + `r1`/`r2` |

帧序列每 200ms 推进一帧（来自 `update_talk_avatar_ui()`）。

### 6.4 情绪来源：云端 function_call → 设备自动应答

```
云端 LLM 在生成过程中调用 emotion(emotion="happy") 函数
   ↓
[下行] on_convai_message_data 收到 JSON
   ↓
cloud_message_callback() 解析后回调
   ↓
{"type":"response.function_call_arguments.done", "calls":[{"name":"emotion", "arguments":"{...}"}]}
   ↓
【设备端必须应答】
{"type":"conversation.items.create",
 "items":[{"type":"function_call_output", "call_id":"...", "output":"{\"result\":\"success\"}"}]}
   ↓
convai_send_message(engine, json, len)
   ↓
云端继续生成（否则 AI 回合阻塞）
```

**关键**：注释明确写"自动回复 function_call_output（所有 function call 都需要）"，否则云端 LLM 会被卡住。

---

## 7. 收到的消息全景（设备视角）

| 类型 | 来源 | 用途 |
|---|---|---|
| on_convai_event CONNECTED/DISCONNECTED/FAILED/UPDATED | 桥接层 | "● 已连接" 状态灯 + 控制"对话"按钮可见性 |
| on_convai_conversation_status 6 种 | 桥接层 | "对话页文字" + 动画帧类型 |
| on_convai_audio_data G.711A | 云端 TTS | （settings 不播放，只接 convai_bridge） |
| on_convai_message_data JSON | 云端 LLM | 解析 function_call.arguments.emotion |
| on_audio（桥接层内部）| 云端 TTS | 解码 G.711A → PCM → ring buffer → AudioService |

---

## 8. SDK 凭证与默认值（`convai_bridge.c`）

```cpp
#define BRIDGE_DEFAULT_BOT_ID         "your_agent_id"
#define BRIDGE_DEFAULT_PRODUCT_ID     "your_product_id"
#define BRIDGE_DEFAULT_PRODUCT_KEY    "your_product_key"
#define BRIDGE_DEFAULT_PRODUCT_SECRET "your_product_secret"
#define BRIDGE_DEFAULT_DEVICE_NAME    "your_device_name"

#define DEFAULT_STARTUP_CONFIG \
  "{\"config\":{\"llm_config\":{\"system_messages\":[" \
    "\"你的名字叫小荷，你可以帮小朋友解决小烦恼哦。\""
  "]}}, ...}"
```

`convai_create()` 时组装完整 JSON：
```json
{
  "info": {
    "product_id":     "<凭证>",
    "product_key":    "<凭证>",
    "product_secret": "<凭证>",
    "device_name":    "<凭证>"
  },
  "ws": { "audio": { "codec": 0 } }    // 0=G.711A
}
```

---

## 9. 关键 API 调用时序

```
[SDK 启动]
convai_create(info_json, handler, ...)      ← 鉴权 + 创 engine
   ↓
[引擎启动]
convai_start(opt{agent_id, params=cfg_json})  ← 建立会话
   ↓
[SDK 自动启动后台录音线程]
audio_record_thread:
    for {
        AudioService.algostream_read() → PCM 20ms
        G.711A encode → convai_send_audio()
    }
   ↓
[云端响应触发回调]
on_convai_audio_data      ← TTS PCM，bridge 解码 → 播放
on_convai_message_data    ← JSON，bridge 透传 → cloud_message_callback
on_convai_event            ← CONNECTED/DISCONNECTED/...
on_convai_conversation_status ← 6 种状态
   ↓
[用户改配置]
apply_ai_settings()
   ↓
convai_update(engine, new_json)   ← 热更新 LLM/TTS 配置
```

---

## 10. 边缘情况

| 情况 | 处理 |
|---|---|
| 引擎未启动就点 AI 设置 | `apply_ai_settings()` 检测 `!sdk_engine` 直接 return，仅保存 JSON |
| `convai_update()` 失败 | 调 `restore_ai_config()` 回滚 4 个 ID + UI 显示回旧值 |
| 重复按 AI 设置按钮 | 进入 `show_cloud_page()`，不影响引擎 |
| 对话页 BACK 键 | 先退出对话页，回到 AI 配置页；再 BACK 才退出 app |
| 主页面 BACK 键 | 调 `convai_bridge_stop()` + `goldie_exit_app()` |
| `cloud_current_cfg_page` 5 种 | enum 标识当前子页（形象/音色/个性/关系/APIKey），决定 show_xxx_setting 写哪个标题 |
| 切换性别 | `on_ai_setting_changed` 内自动重置音色/个性/关系为该性别默认值 |

---

## 11. 文件结构

| 路径 | 行数 | 作用 |
|---|---|---|
| `main_app.cpp` | 1987 | 业务逻辑、回调、状态机 |
| `main_ui.h` | 841 | 60+ UI view 定义、构造、布局 |
| `assets/` | — | 60+ 张表情 RGB565 图片 |

---

## 12. 可被云端业务复用的关键设计

1. **3 段 system_messages 拼接**（base + personality + relationship）—— 云端可拆分存储
2. **音色-性别-关系 三维矩阵** —— 云端可索引化
3. **function_call emotion 是单向控制**（云端 → 设备），设备必须应答 `function_call_output` 才能继续
4. **5 种情绪帧序列** —— 云端 LLM 可通过 tool calling 驱动硬件表情
5. **失败回滚 UI 状态** —— 云端可参考此模式做设置同步冲突处理

---

## 13. 总结

Settings app 是 **AI 硬件的"控制平面 + 体验预览"**：把所有需要让用户配置的 AI 维度（形象/音色/个性/关系/APIKey）UI 化，并把"AI 在跟你说话时该长什么样"以表情动画的形式内嵌呈现，是后续 AItalk 等完整 AI app 的设计蓝本。

云端业务的核心问题就是：**如何高效分发这些配置？如何在对话中反向控制设备（如 emotion）？**