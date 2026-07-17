# cloud_gateway — ConvAI 云端网关（WebSocket）

> 版本：v0.1.0
> ConvAI 云端网关的独立 C++17 参考实现。设备端通过 **WebSocket**（RFC 6455，
> 子协议 `convai.v1`）接入，传输音频、事件和 function-call 消息。

---

## 特性

- **纯 C++17** — 无第三方依赖（无 Boost、无 OpenSSL、无 WebSocket++）
- **WebSocket 传输** — RFC 6455 服务端；JSON 信封承载控制消息，二进制帧承载音频
- **epoll + 线程池** — I/O 单线程事件循环 + N 个工作线程跑 ASR/LLM/TTS
- **可插拔后端** — 当前内置 *stub* 版 ASR/LLM/TTS，无需外部服务即可端到端跑通
- **附带 C++ 测试客户端** `gateway_test_client`，完成握手 + hello + 音频发送

---

## 快速开始

```bash
# 需要：g++ (C++17)、CMake >= 3.16、Linux/macOS（依赖 epoll）
cd cloud_gateway
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# 终端 A：启动网关（默认 9000）
./build/cloud_gateway --port 9000 --log-level debug

# 终端 B：模拟一个设备
./build/gateway_test_client --host 127.0.0.1 --port 9000 --audio-frames 50
```

预期输出：握手成功 → `hello_ack` → 音频流上行 → 网关下行 `status` /
`function_call` / `text` / 二进制音频。

---

## 命令行参数

| 参数                     | 默认值      | 说明                          |
|--------------------------|-------------|------------------------------|
| `--bind ADDR`            | `0.0.0.0`   | 监听地址                     |
| `--port N`               | `9000`      | 监听端口                     |
| `--workers N`            | `4`         | 工作线程数                   |
| `--max-payload N`        | `65536`     | 最大 WS 帧载荷               |
| `--log-level LEVEL`      | `info`      | debug/info/warn/error        |
| `--asr NAME` / `--llm` / `--tts` | `stub` | 当前只支持 `stub` |

环境变量 `GATEWAY_LOG_LEVEL` 同样生效。

---

## 目录结构

```
cloud_gateway/
├── CMakeLists.txt
├── README.md / README_zh.md
├── docs/cloud_gateway/         # 完整文档
│   ├── index.md
│   ├── architecture.md
│   ├── protocol.md
│   ├── api_reference.md
│   └── deployment.md
├── include/cloud_gateway/      # 公共头
├── src/                        # 实现源码
└── test/                       # 测试客户端
```

---

## 文档导航

完整协议、架构和 API 见 [`docs/cloud_gateway/`](docs/cloud_gateway/index.md)：

| 文档 | 内容 |
|---|---|
| [`index.md`](docs/cloud_gateway/index.md) | 总览与阅读顺序 |
| [`architecture.md`](docs/cloud_gateway/architecture.md) | 模块划分、线程模型、生命周期 |
| [`protocol.md`](docs/cloud_gateway/protocol.md) | 线缆格式、消息类型、时序图 |
| [`api_reference.md`](docs/cloud_gateway/api_reference.md) | C++ 类与函数 |
| [`deployment.md`](docs/cloud_gateway/deployment.md) | 编译、运行、生产化加固 |

---

## 与 `ai-hardware-agent-examples` 设备端的对接

本参考实现采用的消息分类与设备端 `sdk_integration/convai_bridge.c`
完全对齐。要在真实设备上跑：

1. 把 `convai_bridge.c` 中的 `convai_info_t` 指向本网关的 `ws://host:port/`
2. 桥接层既有的 `convai_create / convai_start / convai_send_audio / on_convai_message_data`
   即可命中本网关的 session 流水线，触发 settings app 已实现的
   `emotion` function call。

> 详见 [`docs/cloud_gateway/protocol.md`](docs/cloud_gateway/protocol.md)
> §*Compatibility*。

---

## 生产化

本参考实现**未**包含 TLS、鉴权、限流等生产特性。生产部署请阅读
[`docs/cloud_gateway/deployment.md`](docs/cloud_gateway/deployment.md)。