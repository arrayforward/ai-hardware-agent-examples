# AI Hardware Agent Examples — 项目架构与功能说明

> 版本：v1.0  
> 适用：`D:\ai-hardware-agent-examples` 工程根目录  
> 关联文档：`README.md`（编译指南）、`sdk.md`（SDK 重实现方案）

---

## 1. 项目定位

本工程是 **华为 AI Hardware Agent SDK**（位于工程根目录 `include/convai/` 与 `libs/ws63/libconvai_sdk.a`）在 **WS63 嵌入式开发板** 上的端到端示例固件 —— 也就是 [GoldieOS](D:/goldie-os_pub) 项目的"AI 升级版"：

| 维度 | GoldieOS（参考实现） | 本 Examples |
|---|---|---|
| 设备形态 | 320×240 LCD 智能盒子 | 同款硬件 |
| 系统核心 | App/Service/Driver 三层管理 | 同款框架 |
| 网络传输 | 自有 cloud_service（私有协议） | **ConvAI SDK（UDP/WebSocket 不可靠组包）** |
| AI 能力 | 无 | AI 对话、语音唤醒、TTS 表情、闹钟语音播报 |

Examples 的作用：把 SDK 在真实硬件上跑起来，给开发者展示如何把 AI 能力嵌入嵌入式产品。

---

## 2. 顶层目录结构

```
ai-hardware-agent-examples/
├── CMakeLists.txt              # 顶层 CMake 入口（按 CONVAI_PLATFORM 转发）
├── README.md                   # 编译指南（中文）
├── sdk.md                      # SDK UDP 不可靠组包重实现方案
├── build.bat                   # 一键编译脚本
│
├── include/convai/             # SDK 公共头文件（不动）
│   ├── convai_api.h
│   ├── convai_event.h
│   ├── convai_platform.h
│   └── convai_types.h
│
├── libs/ws63/libconvai_sdk.a   # 预编译 SDK 静态库（链接输入）
│
├── examples/goldieos/          # ★ WS63 综合示例固件
│   ├── CMakeLists.txt          # 子工程 CMake
│   ├── init/                   # 系统入口
│   ├── platform/               # 板级平台适配
│   ├── sdk_integration/        # ★ AI SDK 集成桥接层（核心）
│   ├── services/               # 系统服务（音频/WiFi/NTP/闹钟/星闪...）
│   ├── drivers/                # 硬件驱动（LCD/触控/电池/Codec...）
│   ├── apps/                   # 用户应用（Launcher/AI 对话/录音机...）
│   ├── include/                # 内部头文件（core/services/osal/gui...）
│   ├── third_party/            # 第三方库源码
│   └── tools/                  # 工具链与烧录工具
│
└── build/                      # 编译产物（goldieos.fwpkg）
```

---

## 3. 编译产物与构建链

```
源代码 (.c/.cpp)
    ↓  riscv32-linux-musl-gcc/g++   (RISC-V 交叉编译)
libgoldieos_ws63.a   libopus.a   libconvai_sdk.a
    ↓  ws63_link_v4.exe              (HiSilicon 专用链接器)
goldieos.elf   goldieos.bin
    ↓  ws63_sign_tool.exe            (签名工具)
goldieos.fwpkg                       (最终烧录包，约 3.2 MB)
```

**构建关键命令**（详见 `README.md` 与 `build.bat`）：

```bash
mkdir build && cd build
cmake .. -G "MinGW Makefiles" \
  -DCONVAI_PLATFORM=ws63 \
  -DCMAKE_C_COMPILER=riscv32-linux-musl-gcc \
  -DCMAKE_CXX_COMPILER=riscv32-linux-musl-g++ \
  -DCMAKE_C_COMPILER_WORKS=1 -DCMAKE_CXX_COMPILER_WORKS=1 \
  -DCMAKE_MAKE_PROGRAM=make
make
```

构建过程涉及 3 个静态库 + 1 个专用链接器：
1. **`libgoldieos_ws63.a`** — 本工程业务代码（应用、服务、驱动、SDK 桥接）
2. **`libopus.a`** — Opus 1.6.1 定点版（用于语音编解码）
3. **`libconvai_sdk.a`** — 华为 AI Hardware Agent SDK（带 PAL 抽象层）
4. **`ws63_link_v4.exe`** — 海思 WS63 固件打包器，把三个 `.a` 链接成 `goldieos.fwpkg`

---

## 4. 系统架构总览

```
┌────────────────────────────────────────────────────────────────┐
│                          应用层 (apps/)                        │
│  Launcher · AItalk · alarm · recorder · settings · Story ·    │
│  Walkie-talkie · charging_only · shut_down · animaton_player  │
└────────────────────────────────────────────────────────────────┘
            │  App_t 句柄注册       │  wait_service/get_service
            ▼                       ▼
┌──────────────────────────┐   ┌─────────────────────────────┐
│   App Manager (core/)    │   │  Service Manager (core/)    │
│   - install/run/exit     │   │  - 服务注册表（16 个槽位）  │
│   - 触摸/按键事件分发    │   │  - wait_service 阻塞等待    │
└──────────────────────────┘   └─────────────────────────────┘
                                       │
            ┌──────────────────────────┼──────────────────────────┐
            ▼                          ▼                          ▼
   ┌─────────────────┐    ┌─────────────────┐    ┌─────────────────┐
   │  Audio Service  │    │  WiFi Service   │    │  NTP Service    │
   │  (音频录制/播放) │    │  (STA 连接/AP)  │    │  (网络时间同步) │
   └─────────────────┘    └─────────────────┘    └─────────────────┘
   ┌─────────────────┐    ┌─────────────────┐    ┌─────────────────┐
   │  Event Service  │    │  Alarm Service  │    │  SLE SDP/WTP    │
   │  (按键/广播)     │    │  (闹钟管理)      │    │  (星闪协议)      │
   └─────────────────┘    └─────────────────┘    └─────────────────┘
            │
            ▼
┌────────────────────────────────────────────────────────────────┐
│                SDK 集成层 (sdk_integration/)  ← 本工程核心     │
│  convai_bridge.c    - 桥接 goldieos ↔ ConvAI SDK               │
│  convai_config.c    - 设备凭证读取                              │
│  convai_codec_g711a - G.711 A-law 编解码                        │
└────────────────────────────────────────────────────────────────┘
            │  convai_create/start/stop/send_audio/send_message
            ▼
┌────────────────────────────────────────────────────────────────┐
│             ConvAI SDK (libconvai_sdk.a)                        │
│  + AI 对话/语音识别/语义理解 服务（云端能力）                    │
└────────────────────────────────────────────────────────────────┘
            │
            ▼
┌────────────────────────────────────────────────────────────────┐
│                  Platform Abstraction Layer (PAL)              │
│  OSAL: goldie_osal  |  NetAL: lwIP  |  TLSAL: mbedTLS          │
└────────────────────────────────────────────────────────────────┘
            │
            ▼
┌────────────────────────────────────────────────────────────────┐
│                     硬件驱动层 (drivers/)                       │
│  LCD (ST7789) · Touch (CST816D) · Codec (ES8311) · Battery     │
│  Keyboard · I2C · RTC (PCF8563) · Audio PA · GPIO Ext (AW9523B)│
└────────────────────────────────────────────────────────────────┘
```

---

## 5. 核心子系统详解

### 5.1 系统初始化（`init/system_init.c`）

**入口**：`system_init_entry()` 由 `GOLDIE_INIT_CALL_` 宏注册到 LiteOS 启动表。

**启动流程**：

```c
GOLDIE_INIT_CALL_(system_init_entry)   // LiteOS 启动时自动调用
  ↓
sys_init_Task 线程（栈 4 KB，优先级 24）
  ↓
1. wait_for_drvs()          // 阻塞等待所有驱动注册
2. wait_for_services()      // 阻塞等待所有服务注册
3. convai_bridge_init()      // ★ 初始化 AI SDK 桥接
4. convai_bridge_set_audio_source()  // 注入音频源（8 kHz/单声道/16 bit）
5. detect_power_mode()       // 检测电源模式（普通/仅充电）
6. start_boot_animation()    // 开机动画
7. while(1) 主循环：
     - 监听按键 → 唤起 AItalk/Walkie-talkie
     - 检测 P1_5 → 长按关机
     - 定时器 1 s → 同步 NTP、刷新时间、闹钟检测
     - 电池电量/充电状态 → 广播给 UI
```

**GOLDIE_INIT_CALL_ 宏**：`PLATFORM_TYPE_WS63` 下展开为 `app_run(funx)`，注册 LiteOS 自动调用的回调；`PLATFORM_TYPE_WIN` 下不展开（用于 PC 调试）。

### 5.2 三层管理框架（`include/core/`）

#### 5.2.1 App Manager（`app_manager.h/c`）
- 注册入口：`install_app(App_t*)`，每个 app 用 `GOLDIE_INIT_CALL_` 在静态构造期注册
- 生命周期：`run → suspend/resume → exit`，最多 4 层栈（`MAX_APP_STACK`）
- 事件分发：`report_touch()`、`report_keyvalue()` 把底层输入派发到当前焦点 app
- 应用查询：`get_app_by_name()`、`wait_app()`（阻塞直到 app 注册）

```c
typedef struct App_t {
    char app_name[32];
    void (*h_app_run)(void);
    void (*h_app_exit)(void);
    void (*h_app_suspend)(void);
    void (*h_app_resume)(void);
    void (*h_touch_event)(int, int, int);
    void (*h_keyboard_event)(int, int);
    uint16_t *icon;
    int app_status;
} App_t;
```

#### 5.2.2 Service Manager（`service_manager.h/c`）
- 16 个服务槽位（`MAX_SERVICE_INDEX`），按 `enum SERVICE_INDEX` 编号
- 服务以"接口结构体 + 函数指针"形式提供（OO 风格但纯 C）
- 注册：`register_service(idx, void *svc)`；获取：`get_service(idx)` / 阻塞 `wait_service(idx)`

```c
enum SERVICE_INDEX {
    AUDIO_SERVICE_INDEX = 0,
    EVENT_SERVICE_INDEX,
    WIFI_SERVICE_INDEX,
    SDCARD_SERVICE_INDEX,
    CLOUD_SERVICE_INDEX,     // 已弃用，由 convai_bridge 取代
    AUALGO_SERVICE_INDEX,    // 唤醒词/音频算法
    SLE_SDP_SERVICE_INDEX,
    NTP_SERVICE_INDEX,
    ALARM_SERVICE_INDEX,
    SLE_WTP_SERVICE_INDEX,
    RESERVE_SERVICE_INDEX_4..0,
    MAX_SERVICE_INDEX,
};
```

#### 5.2.3 Driver Manager（`driver_manager.h/c`）
两套 API 并存：

**老式**：函数指针表（`DispDrv`、`Wlan_Drv`、`BatDrv` 等），通过 `register_drv/wait_drv/get_drv` 使用。

**新式（`DRV_CORE`）**：类 Linux 的 `goldie_open/ioctl/read/close` + FD 模型（`driver_core.h`），用于电池、AW9523B GPIO 扩展等。

### 5.3 OSAL — 操作系统抽象（`include/osal/`）

`goldie_osal.h` 提供跨平台接口：
- 线程：`goldie_thread_create/destroy/lock/unlock/set_priority`
- 互斥：`goldie_mutex_init/lock/unlock/destroy`
- 信号量：`goldie_sem_init/wait/post/destroy`
- 时间：`goldie_msleep/gettimeofday`
- 内存：`goldie_malloc/free`

WS63 实现：调用 LiteOS API（通过 `app_init.h/sfcop.h/adc.h`）；WIN 实现：调用 Win32 API（`goldie_os_init.h`）。

### 5.4 GUI 框架（`include/cplus_include/tiny_gui/` + `include/gui/`）

`TinyUI` —— 自研轻量 C++ GUI 框架，所有控件都是 `std::shared_ptr<View>` 派生类：

```
View（基类）
├── Window           顶层窗口
├── FrameView        容器（带可选遮罩）
├── LabelView        文本/图像标签
├── ButtonView       按钮（含边界遮罩）
├── CheckboxView     复选框
├── SwitchView       开关
├── ProgressbarView  进度条
├── SpinnerView      旋转指示器
├── MsgBoxView       消息框
├── TextEditView     文本编辑
├── InputMethodView  输入法
├── ListView / ScrollView / VerticalScrollView
├── DateTimeView / DateTimePickerView
├── ListItemFrameView / CaptionFrameView
└── BatteryView / WifiView / NearLinkView  专用控件
```

所有 view 通过 `Window::addView(view, x, y)` 摆放；`Window::flush(x,y,w,h)` 触发 LCD 局部刷新。事件分发：`goldie_touch_event → Window::handleEvent → 子 view`。

### 5.5 ConvAI SDK 集成（`sdk_integration/`）— **本工程最关键模块**

#### 5.5.1 `convai_bridge.c/h` — goldieos 与 SDK 之间的薄桥
提供 goldieos 风格 API（service 形式注册到 `CONVAI_BRIDGE_SERVICE_INDEX=4`），包装 SDK 调用并维护本地状态：

```c
void convai_bridge_init(void);                  // 注册 service + 默认 config
int  convai_bridge_start(void);                 // convai_create + convai_start
int  convai_bridge_stop(void);                  // convai_stop
int  convai_bridge_restart(void);               // stop + start
int  convai_bridge_send_audio(data, len, info); // convai_send_audio
void convai_bridge_set_audio_source(...);      // 注入 mic（启动后台录制线程）
void convai_bridge_set_startup_config(json);   // 由 settings UI 设置人格
```

**录制/播放流水线**：

```
mic (8 kHz mono 16-bit PCM)
   ↓ AudioService->algostream_read()  20 ms 一帧（640 B）
convai_bridge 接收
   ↓ convai_g711a_encode()           PCM → G.711 A-law
convai_send_audio()
   ↓ [SDK 内部：TCP/TLS → 语音识别 + AI 对话]
on_convai_audio_data / on_convai_message_data 回调
   ↓ convai_g711a_decode()           A-law → PCM
RingBuffer (8 KB, 160 ms 缓冲)
   ↓ play_thread (2 KB 栈, 优先级 21)
AudioService->audio_write() → ES8311 → Speaker
```

#### 5.5.2 `convai_config.c` — 凭证读取
从可执行目录读取 `convai.cfg`（`key=value` 格式），提供 `convai_config_get_product_id()`、`_get_agent_id()` 等查询函数。

#### 5.5.3 `convai_codec_g711a.c` — G.711 A-law 编码
- **编码**：16-bit PCM → 8-bit A-law（13-bit 线性 + log 压缩）
- **解码**：8-bit A-law → 16-bit PCM
- SNR ≈ 13.4 dB（语音质量够用）

#### 5.5.4 `platform/convai_platform_ws63.c` — PAL 实现
实现 SDK 要求的回调集：

| PAL 接口 | 实现 |
|---|---|
| `osal.malloc/free` | `goldie_malloc/free` |
| `osal.get_time_ms` | NTP 校准后的 Unix 毫秒 |
| `osal.sleep_ms` | `goldie_msleep` |
| `osal.mutex_*` | `goldie_mutex_*` |
| `osal.thread_*` | `goldie_thread_create` + 参数包装 |
| `netal.socket_*` | mbedtls net_sockets（**TCP**） |
| `tlsal.tls_*` | mbedtls ssl（**TLS**） |
| `misc.log` | printf + 时间戳 |
| `misc.device_id/uuid/info` | 硬编码字符串 |

**当前 SDK 默认走 WebSocket（TCP/TLS）路径**，`sdk.md` 中已规划重写为 UDP 不可靠组包。

### 5.6 系统服务（`services/`）

| 服务 | 文件 | 职责 |
|---|---|---|
| Audio | `audio_service.h`（头在 core） | 录音/播放/铃声/AEC 算法流控制（封装 I2S + ES8311） |
| WiFi | `wifi_service.h`（头在 core） + `network_components/` | STA 连接/扫描、SoftAP 配置 |
| NTP | `ntp_service.c` + `services/ntp/ntp_service.h` | 网络时间同步，返回 `struct tm` |
| Alarm | `alarm_service.c` + `services/alarm/alarm_service.h` | 多闹钟存储/触发，从 SD 卡加载 |
| Event | `event_service.h`（头在 core） | 按键事件队列 + 进程间广播 |
| SLE SDP | `sle_sdp_service.c` | 星闪（NearLink）服务发现 |
| SLE WTP | `sle_wtp_service.c` | 星闪无线传输 |
| Audio Algo | `aud_algo/aualgo_service.c` | 唤醒词识别 / VAD |
| UDP Shell | `network_components/udp_shell.c` | UDP 调试 shell（开发用） |
| Web Server | `network_components/web_server.c` | 嵌入式 HTTP 服务（OTA/配置） |
| TFTP Server | `network_components/tftp_server.c` | TFTP 文件传输 |

### 5.7 硬件驱动（`drivers/`）

| 驱动 | 文件 | 说明 |
|---|---|---|
| LCD | `lcd/st7789.c`、`gc9d01.c`、`ili9341.c` | SPI LCD（CMake 选 `ST7789_SPI_LCD` / `ILI9341_SPI_LCD`） |
| Touch | `touch/cst816d.c`、`ft6336.c` | I2C 触控 IC |
| Codec | `codec/es8311_drv/es8311.c` | ES8311 音频编解码器（I2C + I2S） |
| Audio PA | `audio_pa/pa_drv.c` | 外部功放使能（`SUPPORT_EXT_GPIO_PA`） |
| Battery | `battery/bat_driver.c` | 电池电量采集 |
| RTC | `rtc/pcf8563.c` | I2C 实时时钟 |
| Keyboard | `keyboard/gpio_keyboard.c`（新板）/ `ext_gpio_keyboard.c`（旧板） | GPIO 按键矩阵 |
| I2C | `i2c/i2c.c` | 软件/硬件 I2C 适配 |
| GPIO Ext | `extern_io/aw9523b.c` | AW9523B 16-bit GPIO 扩展（电源管理、关机检测） |

### 5.8 应用（`apps/`）

| 应用 | 文件 | 状态 | 简介 |
|---|---|---|---|
| `launcher` | `main_app.cpp` + 子视图 | **编译进固件** | 主屏：时间/电量/WiFi/星闪/应用宫格 |
| `animaton_player` | `main_app.cpp` | **编译进固件** | 开机动画 + 通用动效播放器 |
| `shut_down` | `main_app.cpp` | **编译进固件** | 关机动画 |
| `charging_only` | `main_app.cpp` | **编译进固件** | 仅充电模式 UI（关机插电时启动） |
| `settings` | `main_app.cpp` | **编译进固件** | 设置：WiFi / AI 人格 / 头像 |
| `AItalk` | `main_app.cpp` | 注释未编 | AI 对话主界面（表情/眼睛动画 + TTS 播放） |
| `alarm` | `main_app.cpp` | 注释未编 | 闹钟管理（增删/启停/重复日） |
| `recorder` | `main_app.cpp` | 注释未编 | 录音机（含 mp3 播放） |
| `Story` | `main_app.cpp/.c` | 注释未编 | 故事机（mp3 列表播放） |
| `Walkie-talkie` | `main_app.cpp` | 注释未编 | 对讲机（基于 SLE 星闪） |
| `dualscreen_ai` | `main_app.cpp` | 条件编译 | 双屏 AI（GC9D01 屏幕专用） |

注：被注释的应用（`AItalk` 等）只需在 `CMakeLists.txt` 中取消注释即可启用。

### 5.9 第三方库（`third_party/`）

| 库 | 用途 | 备注 |
|---|---|---|
| `opus-1.6.1` | 语音编解码 | CMake 子目录构建，定点版 |
| `mbedtls` | TLS / 加密 | 仅 SDK 通过 PAL 调用 |
| `fatfs-R0.11` | FAT 文件系统 | SD 卡 |
| `webrtc_vad` | 语音活动检测 | 唤醒前 VAD |
| `speex_aec` | 回声消除 | 通话/对讲 |
| `helix` | MP3 解码 | 录音机/故事机 |
| `tflite` + `flatbuffers` + `gemmlowp` + `kissfft` | TensorFlow Lite Micro | 预留，唤醒词本地推理（未启用） |
| `cjson` | JSON 解析 | 配置文件读取 |
| `lwip` | TCP/IP 协议栈 | WS63 网络底层 |

---

## 6. 构建配置详解

### 6.1 编译宏开关（`CMakeLists.txt` add_compile_definitions）

| 宏 | 作用 |
|---|---|
| `PLATFORM_TYPE_WS63` | 目标平台标识 |
| `ST7789_SPI_LCD` | LCD 型号选择 |
| `CONFIG_SUPPORT_ES8311_CODEC` | 启用 ES8311 codec |
| `SUPPORT_EXT_GPIO_PA` | 外部 PA 由 GPIO 控制 |
| `SUPPORT_SLE` | 启用星闪（NearLink） |
| `SUPPORT_BATTERY` | 启用电池管理 |
| `SUPPORT_GPIO_KEYBOARD` | 新板 GPIO 按键 |
| `SUPPORT_CST816D_TOUCH` | 触控 IC 型号 |
| `SUPPORT_PCF8563_RTC` | 外部 RTC |
| `USE_EXT_ASR_CHIP_AC2817` | 外部 ASR 芯片（离线唤醒） |
| `LCM_USE_EXT_GPIO` | LCD 复位用扩展 GPIO |
| `DRV_CORE` | 启用新版 FD 驱动模型 |
| `__EMBEDDED__` | 嵌入式标识（屏蔽 dump 等 PC only 代码） |
| `CONFIG_ROM_COMPILE` | ROM 编译优化 |
| `LWIP_CONFIG_FILE="lwipopts_default.h"` | lwIP 配置 |

### 6.2 RISC-V 编译选项

```cmake
-march=rv32imfc -mabi=ilp32f   # 32-bit RISC-V + FPU
-std=gnu99 -Os                   # C99 + 优化大小
-ffreestanding -nostdlib        # 裸机环境
-fdata-sections -ffunction-sections -Wl,--gc-sections  # 死代码消除
-fno-strict-aliasing -fno-unwind-tables
-msmall-data-limit=0 -mpush-pop # RISC-V 小数据优化
```

### 6.3 链接步骤（`ws63_link_v4.exe`）

```
libgoldieos_ws63.a   ─┐
libopus.a            ─┼── ws63_link_v4.exe ──→ goldieos.elf + goldieos.bin
libconvai_sdk.a      ─┘

再经 ws63_sign_tool.exe 加签名 → goldieos.fwpkg (3.2 MB)
```

链接脚本：`linker.lds` + `data.lds` + `function.lds` + `rom_data.lds`，位于 `tools/build/config/ws63/`。

---

## 7. 资源与内存

### 7.1 内存占用（参考）

| 段 | 大小 |
|---|---|
| Flash（goldieos.bin） | ~3.0 MB |
| 签名固件（goldieos.fwpkg） | ~3.2 MB |
| ELF（含调试符号） | ~15 MB |

### 7.2 主要 RAM 占用

| 用途 | 大小 |
|---|---|
| 各后台线程栈（sys_init/playback/recv_loop...） | ~12 KB |
| 音频环形缓冲 | ~8 KB |
| 单帧 mic 缓冲（20 ms） | 640 B |
| SDK engine 状态 | < 4 KB |
| GUI framebuffer（320×240×2 byte） | 153 KB（外部 PSRAM） |

---

## 8. 系统启动时序

```
LiteOS 启动
   ↓
[C 静态构造期] GOLDIE_INIT_CALL_ 注册以下回调
   - platform_init_entry        （注册 SLE 驱动）
   - system_init_entry          （主初始化线程）
   - 各 app 的 *_entry          （install_app）
   - 各 service 的 *_init       （register_service）
   ↓
LiteOS 调度 sys_init_Task 线程（优先级 24）
   ↓
   1. wait_drv(...) ×N          阻塞等所有驱动
   2. wait_service(...) ×N      阻塞等所有服务
   3. convai_bridge_init()      SDK 平台初始化（注入 PAL）
   4. convai_bridge_set_audio_source(audio, 8k, 1, 16)
   5. 检测电源模式
   ↓
   [POWER_MODE_CHARGING_ONLY]
      → 启动 charging_only app → 轮询 AW9523 P1_5 → 检测到高电平重启
   [POWER_MODE_NORMAL]
      → 启动开机动画
      → 等 WiFi 连接（最多 2 s）
      → 启动 launcher
   ↓
   while(1):
      - 1 Hz 主循环
      - 监听 wakeup 键 → 启动 AItalk
      - 监听 SLE WTP 事件 → 启动 Walkie-talkie
      - 同步 NTP 时间
      - 检测电量/充电状态 → 广播到 UI
      - 闹钟 tick 检查 → 触发铃声 + 启动 alarm app
      - 监听 AW9523 P1_5 低电平 → 启动 shut_down 关机流程
```

---

## 9. 关键文件清单速查

| 关心的问题 | 看哪里 |
|---|---|
| 系统怎么启动 | `init/system_init.c` |
| App 怎么注册和运行 | `include/core/app_manager.h`、`apps/launcher/main_app.cpp` |
| Service 怎么注册 | `include/core/service_manager.h`、`services/*/*.c` |
| SDK 怎么调起来 | `sdk_integration/convai_bridge.c` |
| SDK 怎么跑在 WS63 上 | `platform/convai_platform_ws63.c` |
| 音频怎么录播 | `include/core/audio_service.h`、`drivers/codec/es8311_drv/es8311.c` |
| UI 怎么画 | `include/cplus_include/tiny_gui/view.h`、`apps/*/main_app.cpp` |
| 编译参数 | `examples/goldieos/CMakeLists.txt` |
| 链接脚本 | `tools/build/config/ws63/*.lds` |
| 烧录工具 | `tools/burn/hisi/BurnTool_5.0.39/` |

---

## 10. 当前 SDK 的局限与改进方向

### 10.1 现状
SDK 默认走 **TCP + TLS + WebSocket**（通过 mbedtls PAL），对窄带网络和弱网环境不够友好。`sdk.md` 中已规划重写为 **UDP 不可靠组包** 方案。

### 10.2 待优化点
- 唤醒词本地推理（TFLM 已就绪但未启用，目前靠 `USE_EXT_ASR_CHIP_AC2817` 外部芯片）
- `AItalk` / `Story` / `Walkie-talkie` / `alarm` / `recorder` 应用未编入固件（CMake 中注释），需按需开启
- 部分应用代码含 `goldie_thread_create` 旧 API 与新 C++ `Goldie_Thread` 混用

### 10.3 重新生成固件流程
1. `build.bat`（或手动 cmake + make）
2. 拷贝 `build/examples/goldieos/out/goldieos.fwpkg`
3. 用 `tools/burn/hisi/BurnTool_5.0.39/BurnTool.exe` 烧录：
   - 波特率 921600
   - 设备进入 ISP 模式（按住 RESET 上电）
   - Connect → Select file → Send file
4. 烧录完成后长按开机键运行

---

## 11. 与 goldie-os_pub 的差异

| 维度 | goldie-os_pub（参考） | ai-hardware-agent-examples |
|---|---|---|
| `apps/` 实现 | 全部已实现且链接进固件 | **多数未编译**（CMake 中注释） |
| 云端能力 | `cloud_service` 私有协议 | `convai_bridge` → ConvAI SDK |
| 桥接层 | 无 | `sdk_integration/`（新增） |
| AI 对话 | 不支持 | 完整支持 |
| 启动默认 app | launcher | launcher（部分条件下 dualscreen_ai） |
| 固件大小 | ~3 MB | ~3.2 MB |

examples 工程在 goldie-os_pub 基础上：
1. 抽离 cloud_service 改为 SDK 桥接
2. 精简默认应用集（仅保留 launcher/settings/shut_down/charging_only/animation_player）
3. 为 AI 应用（AItalk、Story、Walkie-talkie）保留完整源码但不默认编译，避免无凭证时启动失败
4. 适配 WS63 的 ConvAI SDK PAL 接口

---

## 12. 常见任务指南

### 12.1 启用一个被注释的 app
编辑 `examples/goldieos/CMakeLists.txt` 中 `WS63_APP_SRC`：
```cmake
# apps/AItalk/*.cpp        →   apps/AItalk/*.cpp
```
然后重新 `make` 即可。

### 12.2 修改默认 AI 人格
编辑 `examples/goldieos/sdk_integration/convai_bridge.c` 中：
```c
#define BRIDGE_DEFAULT_BOT_ID         "your_agent_id"
#define BRIDGE_DEFAULT_PRODUCT_ID     "your_product_id"
#define BRIDGE_DEFAULT_PRODUCT_KEY    "your_product_key"
#define BRIDGE_DEFAULT_PRODUCT_SECRET "your_product_secret"
#define BRIDGE_DEFAULT_DEVICE_NAME    "your_device_name"
```
或在运行时通过 `convai_bridge_set_startup_config(json)` 注入。

### 12.3 修改默认 UDP/TCP 目标地址
原 SDK 通过 `convai_create(config_json, ...)` 中的 JSON 传入。新版 UDP 实现见 `sdk.md`。

### 12.4 调试
- `udp_shell.exe`（在 `tools/`）通过 UDP 远程 shell 连接
- 串口日志（115200 8N1）查看 `printf` 输出
- WS63 烧录工具自带的 BurnTool 可看日志

### 12.5 添加新硬件驱动
1. 在 `drivers/` 下创建 `xxx/xxx_drv.c`
2. 在 `include/core/driver_manager.h` 中添加枚举 + 函数指针表
3. 在 `examples/goldieos/CMakeLists.txt` 的 `WS63_DRIVER_SRC` 加入
4. 注册：`register_drv(INDEX, &my_drv_ops)` 或 `goldie_driver_register(&ops)`

---

> 文档结束。如需了解 SDK 内部重写细节，请阅读 `sdk.md`。