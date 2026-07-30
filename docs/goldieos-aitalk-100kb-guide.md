# goldieos AItalk 协议栈 + ConvAI SDK <100KB 优化实施指南（手把手版）

> **读者设定**：完全不了解本工程的弱 AI / 新手。每个文件都按
> **「修改思路 → 修改方案 → 修改效果」** 三段讲清楚，严格按顺序执行，
> 每步验收通过再进下一步。
>
> **目标**（两个，同时达成）：
> 1. WS63 goldieos 工程的 AItalk 应用，其**协议栈 + SDK 部分内存 < 100KB**
>    （102400 字节），且网络/系统卡顿时丢帧不崩溃。
> 2. 用开源自研实现**替换闭源 `libconvai_sdk.a`**（不是移植别的系统，
>    是在原 goldieos 工程内重写优化）。
>
> **参考经验**：`examples/goldieos/05-memory-optimization-100kb.md`。
> **完成态参考实现**：`examples/goldieos/aitalk/`（本指南描述的代码均已存在，
> 不确定时直接对照该目录）。

---

## 0. 先记住 5 条军规（所有改动都由此推导）

1. **消灭每帧 malloc**：音频缓冲全部用文件级 `static` 数组，长期运行不碎片化、
   峰值可预测。
2. **预算单一事实源**：所有池/栈大小只写在 `aitalk/include/convai_limits.h`，
   文件末尾 `_Static_assert(总量 <= 102400)`——超预算直接编译失败。
3. **卡顿语义**：上行 TX 队列满 → 丢**最老帧**+计数（录音线程永不阻塞）；
   下行 RX 环满 → 丢**最老整帧**（4 字节头+负载为一条消息，
   丢字节会把流式解码器状态撕裂）。
4. **可观测**：`convai_open_mem_report()` 打印每个池的水位/丢帧数，
   上板跑一次就知道该调哪个参数。
5. **API 零改动**：开源引擎的公开函数与闭源 SDK 逐字同名同签名
   （`include/convai/convai_api.h`），应用层和 bridge 不改调用方式。

## 1. 最终架构（实现完应该长这样）

```
apps/AItalk/main_app.cpp          UI + aitalk_core(状态机/情绪/聊天队列 2.1KB)
        │
sdk_integration/convai_bridge.c   录音线程(立体声→左声道 mono PCM16)
                                  播放线程(三态机 IDLE/PRIMING/PLAYING, 静态 8K 环)
        │  同名公开 API (convai_create/start/send_audio/...)
aitalk/src/convai_open.c          开源引擎(TX 队列 8×770B / RX 环 8K /
                                  recv+pump+send 三任务, 全静态池)
        │
aitalk/src/convai_ws_client.c     RFC6455 最小 WS 客户端(mbedtls_net 明文 TCP)
        │
        ▼                convai.v1 网关（Go, D:\dev\router）
```

## 2. 环境准备

- Go 1.22+（跑网关）、MinGW gcc（主机验证）。
- 网关仓库 `D:\dev/router`（convai.v1 WebSocket 网关 + mock 后端）。
- Windows 注意：**本机 9000 端口被 svchost 系统服务占用**，测试网关统一用
  **19000**。检查：`netstat -ano | findstr ":9000"`。
- Windows 无 libopus/pkg-config，网关需小改才能构建（见 §3.14）。

---

## 3. 逐文件实施（思路 / 方案 / 效果）

### 3.1 新建 `aitalk/include/convai_limits.h` —— 预算单一事实源

**修改思路**：优化前各缓冲/栈大小散落在十几个文件里，没人说得清总量。
先把所有尺寸集中到一处并配编译期断言，后面的每一步都从这里取常量——
这是整个优化的"地基"，必须第一个做。

**修改方案**：新建文件，四段内容：
1. SDK 引擎：`CONVAI_TX_FRAMES 8`、`CONVAI_TX_SLOT 768`、
   `CONVAI_RX_RING_BYTES 8192`、`CONVAI_ENC_BUF_BYTES 1024`、
   `CONVAI_DEC_PCM_SAMPLES 2048`、任务栈 8192/4096/4096、WS 缓冲 4096/1536。
2. bridge：播放环 8000、预充阈值 480、录音块 640、播放块 1024、
   JSON 拷贝 2048、startup 配置 2048、录音/播放任务栈各 6144。
3. 应用：消息 `4×512`、UI 线程栈 4096。
4. 汇总宏 + 断言：

```c
#define CONVAI_BUDGET_LIMIT_BYTES  102400
CONVAI_STATIC_ASSERT(CONVAI_BUDGET_TOTAL_BYTES <= CONVAI_BUDGET_LIMIT_BYTES,
                     "convai sub-system memory budget exceeds 100KB");
```

**修改效果**：预算表固化进代码，当前合计 **81936B / 102400B（80%）**。
验收：把任一常量调大 10 倍，编译立即报错（断言生效），测完调回。

### 3.2 新建 `aitalk/src/convai_protocol.c/.h` —— convai.v1 线协议

**修改思路**：协议编解码如果散落在引擎里，设备端和测试端容易各写各的、
产生协议漂移。抽成**零平台依赖**的纯 C 模块（只依赖 cJSON），
WS63 固件 / 主机测试 / E2E 模拟器三方共用同一份。

**修改方案**：
- 文本信封 `{"type","seq","ts","body":{...}}`：
  `convai_proto_build_envelope / parse_envelope / envelope_free`。
- 二进制音频头 13 字节**大端** `op(1)+seq(4)+ts(8)`：
  `convai_proto_audio_hdr_pack/unpack`；
  op 取值 `0x10=Frame 0x11=Start 0x12=End 0x13=Cancel`。
- `convai_proto_status_from_str` 把网关状态串映射到 `convai_status_e`。

**修改效果**：协议实现唯一化。验收（e2e 自测用例）：
pack→unpack 往返 seq/ts 逐位一致；非法 JSON 解析返回 -1 不崩溃。

### 3.3 新建 `aitalk/src/convai_ring.c/.h` —— 下行消息环

**修改思路**：优化前 bridge 的播放环是裸字节环，满了要么阻塞要么任意丢字节；
对 ADPCM/Opus 这类**状态连续**的解码器，丢半个帧会导致后续全部解码错位。
需要一个"按整条消息丢弃"的环。

**修改方案**：每条消息 = 4 字节头（magic 0xA5 + 保留 + len u16 LE）+ 负载，
写入 `convai_ring_push` 时若空间不足则循环驱逐**最老整条**消息；
`drops`/`high_water` 计数供观测。环内存由调用方提供静态 arena
（本模块不碰堆）。

**修改效果**：卡顿最坏情况 = 丢老音频保新音频，解码序列永不撕裂。
验收：64B 环连 push 8 条 10B 消息 → 有丢帧计数、pop 出的长度全是 10、
剩余序列连续。

### 3.4 新建 `aitalk/src/convai_codec.c/.h` + `codec_pcm.c` / `codec_g711.c` / `codec_ima_adpcm.c` —— 编解码注册表

**修改思路**：闭源 SDK 内部编解码不可见；新引擎需要可插拔编解码层，
与网关 `hello.audio_codec` 协商一致，且每种格式的内存可记账。

**修改方案**：统一虚表 `convai_codec_t`（id/name/sample_rate/state_size/
init/encode/decode/deinit/mem_usage），注册表注册
PCM16(0)/G711A(1)/G711U(2)/IMA-ADPCM(3)；Opus(4) 用
`#ifdef CONFIG_CONVAI_ENABLE_OPUS` 包裹默认不编译。
`convai_codec.c` 顶部对 `sdkconfig.h` 加 `#if __has_include` 保护
（goldieos 没有该文件）。G.711A 无状态（state_size=0，零内存）；
ADPCM 状态 16B。

**修改效果**：编解码内存显式化（G.711A 模式 = 0 字节实例内存），
运行时可 `convai_open_set_codec()` 切换并被网关 `hello_ack` 协商覆盖。
验收：`convai_codec_get(CONVAI_CODEC_G711A)` 返回 name="g711a"、sr=8000。

### 3.5 新建 `aitalk/src/convai_codec_g711a.c/.h`，删除 `sdk_integration/convai_codec_g711a.c/.h` —— G.711A 去重

**修改思路**：原工程 `sdk_integration/convai_codec_g711a.c` 的 ITU-T 实现
本身没问题（测试向量与网关一致），直接复用。但新 SDK 也带了一份，
两份会**重复定义符号**——必须全工程只留一份。

**修改方案**：实现保留在 `aitalk/src/`（引擎 `codec_g711.c` 的 A-law 适配器
调它），**删除** `sdk_integration/` 下的 `.c/.h` 两个文件。

**修改效果**：链接无 `multiple definition`；静音样本 0 编码 = **0xD5**
（网关测试向量），4 样本编码→解码往返长度一致。

### 3.6 修改 `include/convai/convai_types.h`（仓库根） —— 音频格式枚举增补

**修改思路**：闭源头文件只定义了 `CONVAI_AUDIO_DATA_TYPE_G711A=0`。
开源引擎要在回调里告诉应用"这是解码后 PCM16"，需要补枚举值。
纯增量修改，不破坏旧代码。

**修改方案**：

```c
typedef enum {
    CONVAI_AUDIO_DATA_TYPE_G711A     = 0,
    CONVAI_AUDIO_DATA_TYPE_PCM16     = 1,   /* 新增 */
    CONVAI_AUDIO_DATA_TYPE_G711U     = 2,   /* 新增 */
    CONVAI_AUDIO_DATA_TYPE_IMA_ADPCM = 3,   /* 新增 */
    CONVAI_AUDIO_DATA_TYPE_OPUS      = 4,   /* 新增 */
} convai_audio_data_type_e;
```

**修改效果**：引擎 `on_convai_audio_data` 回调可标注 PCM16。
**坑**：该枚举（G711A=0）与编解码 id（G711A=1）编号不同！引擎判断
"调用方送来的是否已编码数据"时必须显式 switch 映射（见 3.8）。

### 3.7 新建 `aitalk/src/convai_ws_client.c/.h` —— 最小 WebSocket 客户端

**修改思路**：闭源 SDK 自带 WS 传输（且原平台层 TLSAL 是 stub，本来就走明文）。
替换它需要一个满足嵌入式约束的最小 RFC6455 客户端：无每帧 malloc、
缓冲定界、防内存放大。

**修改方案**：
- 传输 `mbedtls_net_context`（WS63 = lwIP 明文 TCP）。
- 握手校验 `HTTP/1.1 101`，子协议 `convai.v1`。
- **客户端发送必须 mask**（随机 4 字节掩码，RFC 强制）；ping 自动回 pong。
- 每连接 `rx=4096B / tx=1536B`，connect 时一次性分配，之后零 malloc；
  单帧超过 rx 缓冲直接断开。
- 阻塞模型 + `convai_wsc_poll(timeout_ms)`，由引擎 recv 任务驱动。

**修改效果**：WS 传输内存定界在 5.6KB/连接；与 gorilla/websocket 网关
互操作验证通过（E2E）。

### 3.8 新建 `aitalk/src/convai_open.c/.h` —— 开源引擎（替换闭源 SDK 的核心）

**修改思路**：闭源 `libconvai_sdk.a` 内存内部不可见、不可控、不可调。
用同一线协议（convai.v1）自研引擎，**公开 API 与 `convai/convai_api.h`
逐字同名同签名**，上层零改动换引擎；内存全部静态定界 + 可观测。

**修改方案**（要点，照抄完成态）：
- **静态池**：`s_enc_buf[1024]`、`s_dec_pcm[2048]`（样本）、
  `s_rx_arena[8192]`；TX 队列（8×770B）内嵌 engine 结构体，
  `convai_create` 时一次性分配。
- **三任务**（goldie_osal 线程）：
  - `recv_task`：`convai_wsc_poll` → 文本（hello_ack/status/event/text）
    / 二进制（0x10 解码入 RX 环；0x11→ANSWERING；0x12→ANSWER_FINISHED）。
  - `send_task`：TX 队列 → 13B 头 + 编码负载 → ws 二进制帧。
  - `pump_task`：RX 环 → `on_convai_audio_data`（**解码后 PCM16**）。
- **协议流程**：connect 后发 `hello`（product_* + audio_codec + sample_rate）；
  `hello_ack` → CONNECTED + LISTENING + 自动 `config_update` 下发人设。
- **卡顿语义**：`tx_push` 满丢最老 + 计数；RX 环整帧驱逐；
  `convai_open_mem_report()` 打印全部水位。
- **passthrough 映射**（3.6 的坑）：`data_type` 显式 switch 映射到 codec id，
  已编码数据走 `tx_push` 直通，PCM16 走 20ms 切片 + 编码。
- **`convai_stop` 销毁顺序（已踩过的坑）**：

```
1) running=0，post tx/rx 两个 sem
2) goldie_thread_destroy(recv)     ← 必须先删（它可能阻塞在 socket recv）
3) goldie_thread_destroy(pump/send)
4) 最后 convai_wsc_destroy          ← 否则 use-after-free
```

**修改效果**：SDK 内存从"不可知"变为 **46352B 定界**（含三任务栈与 WS 缓冲），
G.711A 实例 0B；`convai_get_version()` = `0.2.0-open-ws63`。
验收：E2E 全绿（§4），mem report 水位合理。

### 3.9 修改 `sdk_integration/convai_bridge.c/.h` —— 静态池化 + 编解码下沉

**修改思路**：优化前 bridge 有三处问题：
① 录音线程启动时 `goldie_malloc` 3 块 640B、播放线程 malloc 1KB
（长期运行碎片风险）；② bridge 自己做 G.711 编码/解码，与引擎编解码层重复；
③ 录音/播放任务栈 8KB 偏大。按军规 1/2 收敛。

**修改方案**（逐条对照完成态）：
1. 录音线程 3 个 malloc → 文件级静态 `g_rec_buf[640]`（立体声采集）、
   `g_rec_mono[320]`（左声道 mono）。
2. **删除录音路径的 G.711 编码**：取左声道组 mono PCM16，以
   `CONVAI_AUDIO_DATA_TYPE_PCM16` 调 `convai_send_audio`（编码在引擎内做）。
3. **删除 `on_audio` 的 G.711 解码**与 `g_pcm_decode_buf[1024]`：
   引擎回调的就是 PCM16，直接写播放环。
4. 播放线程 1KB malloc → 静态 `g_play_buf[1024]`。
5. 任务栈 `0x2000` → `CONVAI_RECORD_TASK_STACK / CONVAI_PLAYBACK_TASK_STACK`（各 6144）。
6. 删除 `#include "convai_codec_g711a.h"` 和 `convai_platform_ws63_init()` 调用。
7. 新增 `convai_bridge_mem_report()`（转调引擎 report），头文件加声明。
8. config JSON 增加 `"ws":{"url":...}`（引擎从 `ws.url` 读服务器地址，
   `server_url` 配置键可覆盖）。

**修改效果**：bridge 稳态内存定界 **27328B**；运行时堆分配次数降为 0
（桌面 dump 的 `#ifndef __EMBEDDED__` 段除外）；重复编解码代码移除 ~1.3KB RAM
+ 一份代码体积。
验收：`grep goldie_malloc convai_bridge.c` 只剩桌面调试段。

### 3.10 删除 `platform/convai_platform_ws63.c/.h` —— 摘掉闭源平台层

**修改思路**：该文件是闭源 SDK 的 OSAL/NetAL/TLSAL 适配层
（TLSAL 本来就是 stub），其中 `convai_platform_init()` 是闭源库内部符号。
开源引擎直接用 `goldie_osal`，平台层成为死代码。

**修改方案**：删除两个文件；bridge 里的调用点已在 3.9 移除
（引擎内保留一个同名空 stub 以防其他旧代码引用）。

**修改效果**：减少 368 行死代码；固件不再依赖闭源符号。

### 3.11 新建 `aitalk/src/aitalk_core.c/.h`，修改 `apps/AItalk/main_app.cpp` —— 应用核心去 GUI + 队列裁剪

**修改思路**：AItalk 里与协议栈相关的逻辑（SDK 状态→播放类型映射、
情绪枚举、聊天广播队列）混在 GUI 代码里，且聊天队列 4×1500B=6KB
远超云端文本实际上限（~500 字符）。把这部分抽成全静态、GUI 无关的核心，
队列按实际上限裁剪。

**修改方案**：
- `aitalk_core.c`：`aitalk_on_sdk_status`（IDLE→SLEEP / ANSWERING→SPEAK /
  其他→SILENCE）、情绪枚举、`aitalk_on_sdk_message`（解析云端
  `function_call {"name":"emotion"}`）、`aitalk_push/pop_chat_msg`
  （4×512B，满丢新与原行为一致）、`aitalk_tick`（动画帧计数）。
- `main_app.cpp`：
  1. 顶部 `#include "aitalk_core.h"`；
  2. 删除 `MsgQueue`/`msgque_mutex`，`add_msg` 改一行 `aitalk_push_chat_msg`；
  3. `PLAY_TYPE_*`/`EMOTION_*` 用 `#define` 别名到 `AITALK_*`（GUI 代码不动）；
  4. `sdk_status_callback` 加 `aitalk_on_sdk_status(status)`；
  5. 新增 `cloud_message_callback` → `convai_bridge_on_message()` 注册，
     退出置 NULL；
  6. `play_task` 改为 `play_type = aitalk_get_play_type(); current_emotion =
     aitalk_get_emotion(); aitalk_tick();`；
  7. `update_status()` 里**删掉** `current_emotion = EMOTION_NEUTRAL`
     （旧代码每 200ms 覆盖云端情绪，是 bug）；
  8. run/exit 配对 `aitalk_init/deinit`、`aitalk_set_sdk_started(1/0)`。

**修改效果**：聊天队列 6000B→2048B（省 ~4KB）；应用核心定界 **2112B**；
情绪链路打通（云端可下发 happy/angry/sad/doubt）。
验收：g++ 语法检查除末尾 `GOLDIE_INIT_CALL_`（WS63 专用宏，主机忽略）
外无 error。

### 3.12 新建 `aitalk/src/unwind_stub.c` —— 工具链链接修复

**修改思路**：WS63 自带 riscv32 musl 工具链的 libgcc/libgcc_eh 全是软浮点
（rv32imc），与 ilp32f 固件 ABI 不兼容；链接时报 `_Unwind_*` 未定义。
这些符号在固件里永远不会执行（没启用 C++ 异常）。

**修改方案**：新建 stub 文件提供 `_Unwind_*` 空实现；链接**不加** `-lgcc_eh`。

**修改效果**：`goldieos.elf`（15MB）/ `goldieos.bin`（3.5MB）链接通过。

### 3.13 修改 `CMakeLists.txt` —— 构建切换到开源 SDK

**修改思路**：把 aitalk/src 编进固件、摘掉闭源库，同时保持
`ws63_link_v4.exe` 打包流程不变。

**修改方案**：
1. include 路径加：仓库根 `../../include`（convai/ 公有头）、`aitalk/include`。
2. 源码 glob 加 `aitalk/src/*.c`（并入 `WS63_SDK_INTEGRATION_SRC`）。
3. `apps/AItalk/*.cpp` 取消注释。
4. 删除 `convai_sdk` IMPORTED 库定义、`target_link_libraries` 里的
   `convai_sdk`、`ws63_link_v4` 库列表里的 `libconvai_sdk.a`。
5. 确认 `platform/convai_platform_ws63.c` 不再被引用。

**修改效果**：`libgoldieos_ws63.a` 自带开源引擎，固件链接命令只剩
`libgoldieos_ws63.a + libopus.a`；`USE_OPEN_CONVAI` 宏分支已移除
（开源是唯一路径，闭源库文件本就不在仓库里）。
验收（无 riscv 工具链时的最低限度语法检查）：

```powershell
gcc -std=gnu11 -fsyntax-only sdk_integration\convai_bridge.c `
  -I sdk_integration -I include -I include\core -I include\osal `
  -I include\services -I include\services\ntp -I include\services\alarm `
  -I include\services\audio -I aitalk\include -I ..\..\include `
  -I third_party\fatfs-R0.11\src        # 退出码 0 即通过
```

### 3.14 修改 `D:\dev\router`（网关仓库） —— Windows 可构建

**修改思路**：网关 opus 编解码用 cgo + 系统 libopus，Windows 没有
pkg-config/libopus，整个网关编译不出来；E2E 只用 g711a，不需要 opus。

**修改方案**：`internal/codec/opus.go` 第一行加 `//go:build cgo`；
新建 `internal/codec/opus_stub.go`（`//go:build !cgo`，
`newOpusCodec` 返回错误）。构建用 `CGO_ENABLED=0`；
依赖下载不通时 `GOPROXY=https://goproxy.cn,direct`。

**修改效果**：Windows 上 `go build ./cmd/router ./cmd/mockbackends` 通过；
g711a/adpcm 等四种软编解码不受影响。

### 3.15 新建 `aitalk/e2e_host/` —— 主机 E2E 工程

**修改思路**：参照 05 文档的 mock 法 E2E——**设备源码原样编译**连真实网关，
只替换两个平台垫片；这样测的就是固件里跑的同一份代码。

**修改方案**：
- `goldie_osal_host.c`：pthread 实现 goldie_osal。
  注意 `goldie_thread_destroy` 对应 `pthread_cancel`，所以引擎的阻塞点
  （sem_wait/usleep）必须落在取消点上（本实现满足）。
- `net_sockets_host.c` + `include/mbedtls/net_sockets.h`：WinSock 实现
  `mbedtls_net_*` 子集；socket 设 `SO_RCVTIMEO=100ms`（否则 poll 超时
  和线程取消都不生效）。
- `e2e_main.c`：自测（ring/g711a/协议/预算断言）+ E2E 场景
  （建引擎→起会话→发 1.2s 正弦 PCM→等完整回答轮→断言状态序列与帧数
  →mem report→stop/destroy）。
- `run_e2e.ps1`：一键构建网关+mock、编译设备模拟器、起服务、跑测试、清理。

**修改效果**：`=== PASS (0 failures) ===`——hello→listening→thinking→
文本回复→answering→62 帧/19840B PCM（两轮 TTS）→answer_finished；
TX/RX 零丢帧。同时产出主机可复现的预算打印（81936B）。

## 4. E2E 运行与判据

```powershell
powershell -File examples\goldieos\aitalk\e2e_host\run_e2e.ps1
```

PASS 判据（全绿）：
1. 自测：ring 回绕/整帧丢弃、g711a 0xD5 向量、信封/音频头编解码、预算断言；
2. E2E：`hello→hello_ack→listening` → mock ASR 出句 → `thinking` →
   LLM 文本 → `answering` → 下行 ~40 帧/轮（0.8s TTS，20ms=320B/帧 PCM）→
   `answer_finished`；
3. mem report 水位可解释。

连不上时排查顺序：① `netstat -ano | findstr ":19000"` 确认是 router.exe
在听（**不是系统服务**——9000 被 svchost 占用是本机著名坑）；
② `out\router.err` 有无 `upgrade failed`；③ `out\mock.log` 三个端口是否起来。

## 5. 常见坑速查

| 坑 | 症状 | 解法 |
|---|---|---|
| 9000 被 svchost 占用 | TCP 能连但立刻被断，网关无日志 | 网关与设备都用 19000 |
| Go 依赖下载 EOF | proxy.golang.org 不通 | `GOPROXY=https://goproxy.cn,direct` |
| Windows 无 pkg-config | router 编译失败 | §3.14 build tag + `CGO_ENABLED=0` |
| G.711A 两处定义 | `multiple definition` | §3.5 只留 aitalk 一份 |
| 先释放 ws 再杀线程 | 偶发崩溃 | §3.8 stop 销毁顺序 |
| 单行 if 插桩 | 逻辑莫名失败 | `if (x) goto fail;` 插桩必须加 `{}` |
| PowerShell 改写源文件 | `illegal UTF-8` | 用 `[IO.File]::ReadAllBytes/WriteAllBytes` |
| data_type 与 codec id 混用 | 已编码帧被二次编码 | §3.6 显式映射 |
| libgcc 软浮点 | `_Unwind_*` 未定义 | §3.12 unwind_stub.c |

## 6. 最终验收 Checklist

- [ ] `convai_limits.h` 断言生效，预算合计 ≤ 102400（当前 **81936B**）
- [ ] 主机自测 + E2E `PASS (0 failures)`
- [ ] `convai_bridge.c` 内 `goldie_malloc` 只剩 `#ifndef __EMBEDDED__` 调试段
- [ ] 全工程只剩一份 `convai_g711a_encode/decode`
- [ ] CMake 不再引用 `libconvai_sdk.a` / `convai_platform_ws63.c`
- [ ] （有工具链时）固件 `goldieos.elf/bin` 链接成功
- [ ] （上板后）跑一次 `convai_bridge_mem_report()`，按水位微调 limits
