# AItalk / ConvAI SDK <100KB 内存优化方案（WS63 goldieos 原版）

> 目标：在**原 WS63 goldieos 工程**内，把 AItalk 应用的协议栈与 SDK 部分
> 内存占用优化到 100KB 以下，并用开源实现替换闭源 `libconvai_sdk.a`。
> 本文是一步一步的执行方案，每一步都有代码位置和验证方式。
>
> 代码位置：`goldieos/aitalk/`。验证状态：**已完成**（E2E 两场景 PASS，
> WS63 固件 goldieos.elf/bin 链接成功）。

## 0. 优化目标与结论先行

| 范围 | 优化前 | 优化后 | 手段 |
|---|---|---|---|
| AItalk 消息队列 | 4×1500B = 6000B | 4×512B = 2048B | 消息长度按云端文本实际上限裁剪 |
| ConvAI SDK | 闭源 libconvai_sdk.a（内部不可见、不可控） | 开源 `aitalk/src/convai_open.c`，静态定界 | 重写，convai.v1 协议 |
| 音频缓冲 | bridge 内 malloc + 固定环 | TX 队列 + RX 消息环（静态，丢最老整帧） | 卡顿不崩溃、可观测 |
| 总内存（协议栈+SDK+AItalk） | 不可知 | **G.711A 模式 ~50KB / Opus 模式 ~83KB** | 预算表见 §4 |

## 1. 一步一步实施方案

### Step 1：抽取并复用经过验证的纯 C 模块

从 ai_ws_esp32 项目（同协议 ESP32 实现，35 项主机测试验证过）复制**零平台依赖**模块：

| 文件 | 作用 |
|---|---|
| `src/convai_protocol.c` + `include/convai_protocol.h` | convai.v1 信封 `{type,seq,ts,body}` + 13 字节大端音频头（op/seq/ts）编解码 |
| `src/convai_ring.c` + `include/convai_ring.h` | 消息环形缓冲：满则丢**最老整帧**（4B 头+负载为一条消息），水位/丢帧计数 |
| `src/convai_codec.c`、`codec_pcm.c`、`codec_g711.c`（A/U）、`codec_ima_adpcm.c`、`convai_codec_g711a.c` | 编解码注册表 + 4 种格式（G.711A 直接复用原 goldieos 实现） |

**要点**：这些文件只依赖 cJSON 和 libc（goldieos 自带 `third_party/cjson`），
WS63/ESP32/PC 三处同一份代码，杜绝两侧协议漂移。

### Step 2：WS63 轻量 WebSocket 客户端（`src/convai_ws_client.c`）

替代闭源 SDK 内部的 WS 传输（原平台层 TLSAL 是 stub，本来就明文 ws）：

- 传输：`mbedtls_net_context`（lwIP 明文 TCP）。
- 握手：`GET path HTTP/1.1` + `Sec-WebSocket-Protocol: convai.v1`，校验 `HTTP/1.1 101`。
- 帧：客户端**必须 mask**（随机 4 字节掩码）；支持 text/binary/ping(auto pong)/close。
- 内存：每连接 rx 4KB + tx 1.5KB（连接时一次性分配，之后零 malloc）。
- 阻塞模型 + `convai_wsc_poll(timeout)`，由引擎接收线程驱动。

### Step 3：开源引擎（`src/convai_open.c`）——API 与闭源 SDK 完全同名

**关键决策**：实现 `convai_create/start/stop/update/send_audio/send_message/get_version/err_2_str`
（签名与 `include/convai/convai_api.h` 逐字一致），因此 `sdk_integration/convai_bridge.c`
和 AItalk 应用**零改动**即可换用开源引擎。

线程模型（goldie_osal）：

```
录音线程(convai_bridge) → convai_send_audio → 20ms 切片 → 编码
   → TX 静态队列(8×770B) → send 任务 → ws 二进制帧(13B头+G.711A)
recv 任务 → convai_wsc_poll → 文本(hello_ack/status/event/text) / 二进制
   → 解码 → RX 消息环(8KB) → pump 任务 → on_convai_audio_data(PCM16)
```

- 握手后自动发 `hello`（product_* + audio_codec + sample_rate）；
  `hello_ack` → CONNECTED + LISTENING → 下发 `config_update`（人设）。
- **卡顿语义**：TX 满丢最老帧（录音永不阻塞）、RX 满丢最老整帧（解码状态不撕裂），
  全部计数 + 高水位，`convai_open_mem_report()` 可打印。
- 平台层替代：`platform/convai_platform_ws63.c`（依赖闭源符号）在 open 模式下不编译，
  `convai_platform_ws63_init()` 由引擎内 stub 提供（convai_bridge.c 调用点不动）。

### Step 4：构建切换（CMakeLists，`USE_OPEN_CONVAI`，默认 ON）

```cmake
if(USE_OPEN_CONVAI)                       # 默认 ON
    # aitalk/src/*.c 编进 libgoldieos_ws63.a，不再链接 libconvai_sdk.a
else()                                    # OFF = 回退闭源 SDK
    add_library(convai_sdk STATIC IMPORTED ...)
endif()
```

- open 模式：`WS63_LIB_SDK=""`（ws63_link_v4 的库列表不含闭源库，避免符号重复）。

### Step 5：AItalk 应用优化（`apps/AItalk/main_app.cpp` + `aitalk/src/aitalk_core.c`）

1. **消息队列裁剪**：`MAX_CHAT_MSG_LEN 1500→512`（云端文本回复上限），6000B→2048B；
   队列逻辑收敛到 `aitalk_core.c`（`aitalk_push/pop_chat_msg`，满丢新与原行为一致）。
2. **协议栈核心去 GUI**：play_type 状态机（IDLE→SLEEP/ANSWERING→SPEAK/其他→SILENCE）、
   情绪枚举与动画帧计数（sleep 8 帧/silence 15 帧/happy 往返/angry,sad,doubt 循环）
   移入 `aitalk_core.c`；`main_app.cpp` 用宏别名保持原名兼容，GUI 控件调用不变。
3. **情绪链路补全**：云端 `function_call {"name":"emotion"}` 由
   `aitalk_on_sdk_message` 解析并更新情绪，按协议回 `function_call_output`。

### Step 6：WS63 特有坑的修复（本工程已处理）

| 坑 | 修复 |
|---|---|
| 头文件反斜杠路径 `platform\ws63\errcode.h`（Linux 不可编译） | `include/platform/ws63/bts/sle/sle_common.h`、`sle_ssap_client.h` 改为正斜杠 |
| libopus.a ABI 不匹配（add_subdirectory 早于全局 -mabi 设置，编成软浮点） | opus 子目录前显式 `CMAKE_C_FLAGS += -march=rv32imfc -mabi=ilp32f` |
| 工具链 libgcc/libgcc_eh 全为软浮点（rv32imc），与 ilp32f 固件无法链接 | 链接去掉 `-lgcc_eh`（例外未启用），加 `aitalk/src/unwind_stub.c` 提供 `_Unwind_*` 符号（永不执行） |
| `ws63_link_v4.exe` 仅 Windows 且要调用 Linux gcc | 在 WSL 手工复现其链接命令（见 §5） |

## 2. 验证（全部通过）

### 2.1 端到端（Go 网关 + mock 后端，与本机真实 ASR/TTS 同协议）

```bash
# 网关仓库: go-esp32-ws-server
./bin/mockbackends -asr :51051 -llm :51052 -tts :51061 &
./bin/router -listen :9000 -asr 127.0.0.1:51051 -llm 127.0.0.1:51052 -tts 127.0.0.1:51061 &
# WS63 开源 SDK 的主机版（aitalk/src + e2e/shim，Linux gcc 编译）
gcc ... goldieos/aitalk/e2e/e2e_main.c ... -o ws63_e2e && ./ws63_e2e
```

结果：**g711a / ima_adpcm 两场景 PASS**——hello→listening→30 帧上行→
thinking→AI 文本→answering→40 帧 TTS→answer_finished；TX/RX 零丢帧。

### 2.2 WS63 固件编译链接（compile proof）

```bash
# riscv32 musl 工具链（仓库自带 Linux 版）
cmake -S goldieos -B build_open -DCMAKE_C_COMPILER=riscv32-linux-musl-gcc \
      -DCMAKE_CXX_COMPILER=riscv32-linux-musl-g++ -DUSE_OPEN_CONVAI=ON
cmake --build build_open --target goldieos_ws63     # libgoldieos_ws63.a ✔
# 手工链接（等价 ws63_link_v4 内部命令，见 §5）
# → out/goldieos.elf (15MB) + objcopy → goldieos.bin (3.5MB) ✔
```

## 3. 内存预算表（AItalk 协议栈 + 开源 SDK，<100KB）

| 池 | 大小 | 位置 |
|---|---|---|
| WS 客户端缓冲（rx 4K + tx 1.5K，每连接一次） | 5.5 K | convai_ws_client.c |
| 引擎结构体 | ~1 K | convai_open.c |
| 静态音频池（enc 1K + dec 4K + TX 8×770B + RX 8K） | 19.2 K | convai_open.c 静态区 |
| 任务栈（recv 8K + pump 4K + send 4K） | 16 K | convai_open.h 常量 |
| 编解码实例（G.711A=0 / ADPCM=16B / Opus 懒加载 ≤33K） | 0~33 K | codec_*.c |
| JSON 工作集 | ~2 K | cJSON |
| AItalk 核心（4×512 + 状态）+ play_thread 栈 4K | 6.1 K | aitalk_core.c / main_app.cpp |
| **合计（G.711A 模式）** | **≈ 50 K** ✅ | |
| **合计（Opus 模式）** | **≈ 83 K** ✅ | |

后续 wss（mbedtls_ssl）启用需 +~40K：G.711A 模式仍在 100K 内，
Opus 模式需把 RX 环降到 4K + TX 降到 6 帧（Kconfig 化即可，见 convai_open.h 常量）。

## 4. 使用方式

```cmake
# 默认已是开源 SDK。回退闭源：
cmake -DUSE_OPEN_CONVAI=OFF ...
```

- AItalk 应用与其他 App 无需任何改动（API 完全同名）。
- 运行时诊断：`convai_open_mem_report()` 打印各池水位/丢帧。
- 网关侧用 `go-esp32-ws-server`（支持全部 5 种编解码与 wss）。

## 5. 附录：WSL 中复现 ws63_link_v4 的链接命令

`ws63_link_v4.exe` 是 Windows PE 且内部 shell 调用 Linux riscv-gcc（WSL 互操作无法生成子进程），
等价手工命令（从该 exe 日志还原，本项目验证通过）：

```bash
riscv32-linux-musl-g++ -march=rv32imfc -mabi=ilp32f @tools/build/config/ws63/rom_cb_flag.srp \
  -o out/goldieos.elf -Wl,--cjal-relax -Wl,--dslf -Wl,--gc-section -nostdlib -static \
  -Wl,--enjal16 -g -Wl,--just-symbols=tools/build/config/ws63/acore.sym \
  -T linker.lds -T data.lds -T function.lds -T rom_data.lds \
  -L libs/ws63/board -L libs/ws63/stdlib -L libs/ws63/board/wifi-protocol \
  -Wl,--whole-archive libgoldieos_ws63.a opus_build/libopus.a \
  <WS63 板级 -l 列表（见 build_open 日志）> \
  -Wl,--no-whole-archive \
  libs/ws63/stdlib/{libc,libstdc++,libsupc++,libm,libgcc}.a
riscv32-linux-musl-objcopy -O binary out/goldieos.elf out/goldieos.bin
```

注意：**不要**加 `-lgcc_eh`（工具链软浮点版本），改用 `aitalk/src/unwind_stub.c`。
