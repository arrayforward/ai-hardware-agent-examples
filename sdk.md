# AI Hardware Agent SDK — UDP 不可靠组包重实现 设计方案

> 版本：v1.0
> 目标：在保留公开 API/ABI 兼容的前提下，用 UDP 替换原 WebSocket 传输，并使用"尽力而为"的组包策略——任一包失败即丢弃整条消息。

---

## 1. 设计目标与约束

### 1.1 目标
- 保留 `include/convai/*.h` 全部头文件不变，保证 `sdk_integration/convai_bridge.c` 及上层 app **零修改**即可切换到新 SDK
- 用 UDP 替代 WebSocket 传输
- 实现"不可靠组包"语义：消息被切成多个 UDP 报片，任一片 `sendto` 失败 → 立即停止，剩余片不再发送，整条消息视为丢弃
- 适配 WS63（RISC-V / musl / LiteOS）嵌入式环境

### 1.2 约束
- 不修改 `ws63_link_v4.exe` 工具本身
- 不修改 `libs/ws63/stdlib/`、`libs/ws63/board/` 提供的板级 `.a`
- 不新增第三方依赖（mbedtls、opus、fatfs 已就绪，但本方案用不到）
- 所有时间/内存/线程/socket 调用必须走 PAL（`convai_platform.h`）

---

## 2. 公开接口回顾（保留不变）

| 分类 | 函数 | 行为变化（相对原 SDK） |
|---|---|---|
| 生命周期 | `convai_create/destroy` | 无 |
| 会话 | `convai_start/stop/update` | `start` 改为创建 UDP socket 并缓存目标地址；不发起 TCP/TLS 握手 |
| 数据 | `convai_send_audio/send_message` | 走 UDP 不可靠组包 |
| 工具 | `convai_get_version` / `convai_err_2_str` | 无 |
| 平台 | `convai_platform_init` + 4 个 accessor | 必须实现，否则内部无法取 OSAL/NetAL |

新增枚举值（向后兼容）：
```c
typedef enum {
    CONVAI_MODE_WS  = 0,
    CONVAI_MODE_UDP = 1,   // 新增
} convai_mode_e;
```

---

## 3. 总体架构

```
┌──────────────────────────────────────────────────────────────┐
│                    goldieos app (convai_bridge.c)             │
└──────────────────────────────────────────────────────────────┘
                              │  调用公开 API
                              ▼
┌──────────────────────────────────────────────────────────────┐
│              convai_api (实现的 9 个导出符号)                 │
│  convai_create / destroy / start / stop / update /           │
│  send_audio / send_message / get_version / err_2_str         │
└──────────────────────────────────────────────────────────────┘
        │                       │                       │
        ▼                       ▼                       ▼
┌───────────────┐   ┌──────────────────┐   ┌──────────────────┐
│ convai_packet │   │   convai_udp      │   │ convai_platform  │
│  切片/组包    │   │ socket+sendto    │   │   PAL 注入/获取  │
└───────────────┘   └──────────────────┘   └──────────────────┘
                                                  │
                                                  ▼
                                       ┌──────────────────┐
                                       │   convai_osal    │
                                       │  NetAL / OSAL    │
                                       │  (PAL 回调)      │
                                       └──────────────────┘
```

---

## 4. UDP 包格式

### 4.1 包头（固定 12 字节，网络字节序）

| 偏移 | 长度 | 字段 | 说明 |
|---|---|---|---|
| 0 | 1 | `magic` | `0xC1`，对端校验；非此值直接丢弃 |
| 1 | 1 | `type` | 包类型（见 4.2） |
| 2 | 2 | `seq` | 当前包在消息内的序号（从 0 开始） |
| 4 | 2 | `total` | 整条消息总包数 |
| 6 | 4 | `msg_len` | 整条消息载荷总长（字节，仅 `FIRST` 包携带有效） |
| 10 | 2 | `payload_len` | 本包载荷长度 |
| 12 | 变长 | `payload` | 分片数据，≤ `MAX_PAYLOAD` |

### 4.2 包类型 `type`

| 值 | 名称 | 含义 |
|---|---|---|
| 0x01 | `PKT_HELLO` | `convai_start` 时发出，载荷为 `convai_info_t` JSON |
| 0x02 | `PKT_BYE` | `convai_stop` 时发出，载荷为空 |
| 0x03 | `PKT_CONFIG` | `convai_update` 时发出，载荷为 JSON |
| 0x10 | `PKT_AUDIO_FIRST` | 音频消息首片，载荷为 G.711A 分片 |
| 0x11 | `PKT_AUDIO_MID` | 音频消息中间片 |
| 0x12 | `PKT_AUDIO_LAST` | 音频消息末片 |
| 0x20 | `PKT_MSG_FIRST` | 文本消息首片 |
| 0x21 | `PKT_MSG_MID` | 文本消息中间片 |
| 0x22 | `PKT_MSG_LAST` | 文本消息末片 |
| 0x80 | `PKT_SERVER_PUSH` | 服务端下行（语音/文本/事件），载荷为完整帧或 JSON |

### 4.3 配置参数

```c
#define CONVAI_LOCAL_UDP_MTU        1200   // 单包载荷上限
#define CONVAI_LOCAL_HDR_SIZE       12
#define CONVAI_LOCAL_MAX_PACKETS    256    // 单消息最大片数（16bit 限制）
#define CONVAI_LOCAL_DEFAULT_PORT   9999
#define CONVAI_LOCAL_RECV_TIMEOUT_MS 50    // 单次 recv 超时
```

---

## 5. 关键流程

### 5.1 `convai_send_audio` 不可靠组包（核心）

```
int convai_send_audio(handle, data, len, info):
    if len == 0            → return CONVAI_OK
    计算 total = ⌈len / MAX_PAYLOAD⌉
    if total > MAX_PACKETS → return CONVAI_ERR_INVALID_PARAM

    for seq in [0, total):
        payload_len = min(MAX_PAYLOAD, len - seq*MAX_PAYLOAD)
        fill header: magic/type(seq)/seq/total/msg_len/payload_len
        n = socket_sendto(sock, pkt, HDR+payload_len, &peer)
        if n < 0 OR n != HDR+payload_len:
            // 不可靠：丢弃剩余包
            log(WARN, "udp fragment %d/%d failed, drop msg", seq, total)
            return CONVAI_ERR_NETWORK
    return CONVAI_OK
```

**不重试、不排队、不缓存**。任一片失败即视为整条消息失败，应用层需要自己处理重传或丢包容忍。

### 5.2 `convai_send_message`
逻辑同 5.1，包类型用 `PKT_MSG_FIRST/MID/LAST`。

### 5.3 `convai_start`
1. 若 engine 已 started → 返回 `CONVAI_ERR_ALREADY_STARTED`
2. 解析 `config_json` 提取 `udp_host` / `udp_port`（缺省用 `convai_local_config.h` 中的默认值）
3. `socket_create(&sock)`；保存对端 sockaddr
4. 组装 `convai_info_t` JSON，封装成单包 `PKT_HELLO` `sendto`
5. 启动后台接收线程（可选，见 5.6）
6. 调用 `handler.on_convai_event(CONNECTED)`
7. 调 `handler.on_convai_conversation_status(CONVAI_STATUS_IDLE)`
8. 返回 `CONVAI_OK`

### 5.4 `convai_stop`
1. `sendto(PKT_BYE)`
2. 关闭接收线程（若有）
3. `socket_destroy(sock)`
4. 触发 `DISCONNECTED` 回调
5. 返回 `CONVAI_OK`

### 5.5 `convai_update`
- 单包 `PKT_CONFIG`，载荷即 `session_update_json`
- sendto 失败 → `CONVAI_ERR_NETWORK`，但不中断会话

### 5.6 接收（轻量）
两条路线，按平台能力二选一：

**路线 A（推荐，WS63）**：在 `convai_start` 中创建短栈线程 `_recv_loop`，循环：
```c
while (running) {
    n = socket_recv(sock, buf, sizeof(buf), &recvd);
    if (n <= 0) { sleep_ms(50); continue; }
    dispatch_packet(buf, recvd);   // 校验 magic、按 type 调回调
}
```
- 不重组、不去重
- `PKT_SERVER_PUSH` 载荷为完整音频帧或 JSON 文本，按内嵌字段判断后调 `on_convai_audio_data` / `on_convai_message_data` / `on_convai_event`

**路线 B（无线程）**：在 `convai_send_*` 之前先 `socket_recv` 一次非阻塞，调用方负责节奏。简单但响应不及时，仅作 fallback。

### 5.7 错误映射
| 触发 | 返回码 |
|---|---|
| 任一 UDP 片 sendto 失败 | `CONVAI_ERR_NETWORK` |
| socket 创建失败 | `CONVAI_ERR_PLATFORM` |
| handle 为 NULL / 已 destroy | `CONVAI_ERR_INVALID_PARAM` 或 `CONVAI_ERR_NOT_INITIALIZED` |
| start 后再 start | `CONVAI_ERR_ALREADY_STARTED` |
| stop 前未 start | `CONVAI_ERR_NOT_STARTED` |

---

## 6. 状态机

```
          convai_create
                │
                ▼
            ┌────────┐  start  ┌──────────┐  stop   ┌──────────┐
            │ INIT   │ ──────▶ │ STARTED  │ ──────▶ │ STOPPED  │
            └────────┘         └──────────┘         └──────────┘
                │                    │   ▲                │
                │ destroy            │   │ restart        │ destroy
                ▼                    ▼   │                ▼
            ┌────────┐         ┌──────────┐         ┌────────┐
            │ DESTROYED        │ FAILED   │         │ DESTROYED
            └────────┘         └──────────┘         └────────┘
```

非法迁移全部返回 `CONVAI_ERR_INVALID_STATE`。

---

## 7. 线程模型

| 线程 | 栈 | 创建时机 | 职责 |
|---|---|---|---|
| 调用方线程（mic/app） | — | — | 调用 send_audio / send_message |
| `_recv_loop` | 4 KB | start 时 | 收 UDP 包，分发回调 |
| （未来）`_heartbeat` | 2 KB | 可选 | 周期性 HELLO 保活；本方案不实现 |

PAL `mutex_*` 用于保护 engine 内部状态字段；UDP socket 自身在 musl 下 sendto 原子，不需额外加锁。

---

## 8. 内存预算（WS63）

| 对象 | 大小 | 数量 | 备注 |
|---|---|---|---|
| engine 句柄结构 | ~256 B | 1 | `convai_engine_t` |
| 对端 sockaddr | 16 B | 1 | IPv4 即可 |
| 发送包缓冲 | `MTU + HDR` ≈ 1212 B | 1（栈或静态） | 切片时单次构造 |
| 接收包缓冲 | 1500 B | 1 | 接收线程栈上 |
| 接收线程栈 | 4 KB | 1 | PAL thread_create |
| **总计** | < 8 KB | | |

---

## 9. 文件清单

### 9.1 新增（实现源码）

```
examples/goldieos/sdk_local/
├── CMakeLists.txt                       # 新增
├── include/
│   └── convai_local_config.h            # UDP 默认地址、MTU
└── src/
    ├── convai_sdk.c                     # 9 个公开 API 的实现
    ├── convai_udp.c                     # socket 封装、sendto/recvfrom
    ├── convai_udp.h
    ├── convai_packet.c                  # 包头编/解、MTU 切片
    ├── convai_packet.h
    ├── convai_platform.c                # convai_platform_init + 4 个 accessor
    └── convai_err.c                     # convai_err_2_str
```

### 9.2 修改

| 文件 | 改动 |
|---|---|
| `examples/goldieos/CMakeLists.txt` | 删除 `add_library(convai_sdk STATIC IMPORTED)`；新增 `add_subdirectory(sdk_local)`；`goldieos_ws63` 链接由 `convai_sdk` 替换 `convai_sdk`；`WS63_LIB_SDK` 路径改为 `${CMAKE_BINARY_DIR}/examples/goldieos/sdk_local/libconvai_sdk.a` |

### 9.3 不动

- `include/convai/*.h`、`sdk_integration/*`、`libs/ws63/**`、`tools/**`、顶层 `CMakeLists.txt`、`ws63_link_v4.exe`

---

## 10. CMake 集成示例

```cmake
# examples/goldieos/CMakeLists.txt 关键片段
+ add_subdirectory(sdk_local)

- add_library(convai_sdk STATIC IMPORTED)
- set_target_properties(convai_sdk PROPERTIES
-     IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/libs/ws63/libconvai_sdk.a
- )

  target_link_libraries(goldieos_ws63 PRIVATE
      opus
      convai_sdk
      -lc
  )

  set(WS63_LIB_SDK
-     "${CMAKE_SOURCE_DIR}/libs/ws63/libconvai_sdk.a"
+     "${CMAKE_BINARY_DIR}/examples/goldieos/sdk_local/libconvai_sdk.a"
  )
```

```cmake
# examples/goldieos/sdk_local/CMakeLists.txt
file(GLOB SRC CONFIGURE_DEPENDS src/*.c)
add_library(convai_sdk STATIC ${SRC})
target_include_directories(convai_sdk PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/examples/goldieos/sdk_local/include
)
target_link_libraries(convai_sdk PUBLIC
    goldie_osal     # PAL 回调依赖
)
```

---

## 11. 验证方案

| 阶段 | 验证手段 |
|---|---|
| 编译 | `build.bat` 一键脚本应产出 `goldieos.fwpkg`；`convai_sdk` 在 build 日志中独立编译 |
| 单元（PC host） | 用 MSYS2 gcc 编译 `convai_packet.c` + 简单 stub，写一个 fragment/drop 测试 |
| 联机 | 在 PC 上用 `python -m socket` 或 `udp_shell.exe` 起一个 UDP 监听，板端 `sendto` 后观察是否能收到所有包；以及手动 `iptables -A OUTPUT -p udp --dport 9999 -j DROP` 模拟丢包验证丢弃语义 |
| 回归 | `convai_bridge_init/start/send_audio/stop/destroy` 全流程不应改动 app 源码 |

---

## 12. 风险与对策

| 风险 | 对策 |
|---|---|
| UDP 在某些 NAT 下不可达 | 文档注明"局域网/直连场景使用"，生产部署前评估 |
| 嵌入式栈空间不足 | 接收线程栈设为 4 KB，包缓冲 1500 B 单包处理 |
| `sendto` 在 mic 高频调用下挤占 CPU | 后续可加"攒 N ms 一次 flush"优化；本期不做 |
| 包头版本演进 | 预留 `magic` 后 1 bit 作为 version；本期固定 v1 |
| 与原预编译 SDK 名字冲突 | 不动原 `libs/ws63/libconvai_sdk.a` 实体，CMake 切路径即可 |

---

## 13. 后续可选扩展（非本期范围）

- 添加简单 FEC（前向纠错），提升抗丢包能力
- 周期性 HELLO 心跳 + 超时检测
- 支持 IPv6 sockaddr
- 把接收路径扩展为 DTLS（仍走 PAL 的 TLSAL）