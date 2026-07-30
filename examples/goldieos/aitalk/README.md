# aitalk — WS63 开源 ConvAI SDK + AItalk 协议栈核心

替代闭源 `libconvai_sdk.a` 的开源实现（convai.v1 协议），以及
`apps/AItalk` 的协议栈/SDK 核心（去 GUI）。总内存 <100KB。

## 目录

```
aitalk/
├── AITALK_OPTIMIZATION.md     # 完整一步一步优化方案（先读这个）
├── include/                   # 头文件（协议/编解码/环/引擎/AItalk核心）
├── src/
│   ├── convai_open.c          # 开源引擎（API 与闭源 SDK 完全同名，应用零改动）
│   ├── convai_ws_client.c     # 轻量 WebSocket 客户端（lwIP 明文 TCP，RFC6455）
│   ├── convai_protocol.c      # convai.v1 信封 + 13B 音频头（纯C）
│   ├── convai_ring.c          # 消息环形缓冲（丢最老整帧，纯C）
│   ├── convai_codec.c + codec_*.c + convai_codec_g711a.c   # 编解码注册表与实现
│   ├── aitalk_core.c          # AItalk 协议栈核心（状态机/情绪/512B消息队列）
│   └── unwind_stub.c          # _Unwind_* 链接 stub（工具链软浮点 libgcc 规避）
└── e2e/                       # 主机端 E2E（shim + 场景，连 Go 网关+mock 后端）
    ├── shim/                  # goldie_osal / mbedtls_net 的 Linux 实现
    └── e2e_main.c             # g711a + ima_adpcm 场景（PASS）
```

## 构建开关

```cmake
-DUSE_OPEN_CONVAI=ON   # 默认：开源 SDK（aitalk/src 编入固件）
-DUSE_OPEN_CONVAI=OFF  # 回退闭源 libconvai_sdk.a
```

## 验证

- E2E：`goldieos/aitalk/e2e/`（命令见 AITALK_OPTIMIZATION.md §2.1）
- 固件：libgoldieos_ws63.a + goldieos.elf/bin 链接通过（§2.2）
