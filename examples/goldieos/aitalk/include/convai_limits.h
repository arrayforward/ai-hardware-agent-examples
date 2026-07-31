/**
 * @file convai_limits.h
 * @brief Single source of truth for the ConvAI SDK-layer memory budget on
 *        WS63 goldieos.
 *
 * 预算口径（重要）：**整个 SDK 层 < 100KB** —— 开源引擎 + convai.v1 协议栈
 * + WS/TLS 传输 + convai_bridge 集成层。**不含 AItalk 应用本体**
 * （aitalk_core 状态/队列 + UI 线程栈单独列出，不计入预算）。
 *
 * 参考 05-memory-optimization-100kb.md 的路线:
 *   路线1 消除每帧 malloc (静态工作缓冲)
 *   路线3 静态池 + 丢帧不崩溃 (TX 队列丢最老 / RX 环整帧丢弃)
 *   路线4 TLS 瘦身 (in/out 各 4KB + ECDSA P-256 + AES-256-GCM)
 *   路线5 可观测性 (convai_open_mem_report)
 *
 * All buffers/tasks of the SDK layer are sized HERE and only here.
 * The compile-time assertions at the bottom fail the build when the
 * budget exceeds 100 KB (102400 bytes) — the budget is self-enforcing.
 *
 * Budget table (steady state, G.711A codec — stateless):
 *
 *   | 池                                        | 字节   | 位置                    |
 *   |-------------------------------------------|--------|-------------------------|
 *   | 编码工作缓冲 s_enc_buf                     | 1024   | convai_open.c           |
 *   | 解码工作缓冲 s_dec_pcm (2048 samples)      | 4096   | convai_open.c           |
 *   | 下行 RX 消息环 s_rx_arena                  | 8192   | convai_open.c           |
 *   | 上行 TX 帧队列 8x770B (engine struct)      | 6160   | convai_open.c           |
 *   | 泵任务消息缓冲 (static)                    | 4096   | convai_open.c pump_task |
 *   | 任务栈 rx 8K + pump 4K + tx 4K             | 16384  | convai_open.c           |
 *   | WS 客户端 rx 4K + tx 1.5K (connect 时分配) | 5632   | convai_ws_client.c      |
 *   | engine 结构体其余字段                       | ~768   | convai_open.c           |
 *   | 播放环 g_ring_data                         | 8000   | convai_bridge.c         |
 *   | 录音静态缓冲 640+320B                       | 960    | convai_bridge.c         |
 *   | 播放静态缓冲                                | 1024   | convai_bridge.c         |
 *   | JSON 拷贝缓冲                               | 2048   | convai_bridge.c         |
 *   | startup config 缓冲                        | 2048   | convai_bridge.c         |
 *   | 任务栈 record 6K + playback 6K             | 12288  | convai_bridge.c         |
 *   | cJSON 瞬态工作集                            | ~2048  | cJSON (heap, 瞬态)      |
 *   |-------------------------------------------|--------|-------------------------|
 *   | **SDK 层合计（明文 ws）**                  | **75728** | ✅ < 102400          |
 *   | + WSS：TLS in 4K + out 4K（记录缓冲）      | +8192  | mbedtls (见下方前置条件) |
 *   | + WSS：握手/X.509/DRBG 峰值                | +4096  | mbedtls (握手后回落)     |
 *   | **SDK 层合计（wss, AES-256-GCM）**         | **88016** | ✅ < 102400          |
 *   | （不计入）AItalk 应用本体 4x512+栈           | 6208   | aitalk_core.c/main_app  |
 *
 * WSS 前置条件（mbedTLS 构建配置）：libmbedtls 必须以
 *   MBEDTLS_SSL_IN_CONTENT_LEN=4096 / MBEDTLS_SSL_OUT_CONTENT_LEN=4096
 * 编译（convai 最大帧 <1KB，4KB 裕量充足）。若使用 16KB 默认值的预编译库，
 * WSS 增量将从 12KB 涨到 ~37KB，超出预算 —— 此时必须重编 libmbedtls。
 */
#ifndef CONVAI_LIMITS_H
#define CONVAI_LIMITS_H

/* ================= open SDK engine (convai_open.c) ================= */

/** TX uplink queue depth (8 x 20ms = 160ms of audio). */
#define CONVAI_TX_FRAMES        8
/** Max one encoded 20ms frame payload. */
#define CONVAI_TX_SLOT          768
/** Downlink decoded-PCM jitter ring (0.5s @ 8kHz 16bit mono). */
#define CONVAI_RX_RING_BYTES    8192
/** Encode work buffer (covers 20ms max encoded output of all codecs). */
#define CONVAI_ENC_BUF_BYTES    1024
/** Decode work buffer in samples (oversized frames are dropped). */
#define CONVAI_DEC_PCM_SAMPLES  2048

/** Engine task stacks (bytes). */
#define CONVAI_WS_RX_TASK_STACK   8192   /* recv thread: JSON parse + TLS 握手余量 */
#define CONVAI_PUMP_TASK_STACK    4096   /* downlink pump thread */
#define CONVAI_SEND_TASK_STACK    4096   /* uplink send thread */

/** WebSocket client buffers (allocated once per connect). */
#define CONVAI_WSC_RX_BYTES       4096
#define CONVAI_WSC_TX_BYTES       1536

/* ================= TLS / WSS (convai_ws_client.c) ================= */

/** mbedTLS record buffers — 必须与 libmbedtls 编译配置一致
 *  (MBEDTLS_SSL_IN_CONTENT_LEN / MBEDTLS_SSL_OUT_CONTENT_LEN)。 */
#define CONVAI_TLS_IN_CONTENT_LEN   4096
#define CONVAI_TLS_OUT_CONTENT_LEN  4096
/** Handshake peak: X.509 parse + ECDHE P-256 + CTR-DRBG/entropy 等
 *  (握手后大部分回落, 按峰值记账)。 */
#define CONVAI_TLS_HANDSHAKE_BYTES  4096

/* ================= bridge (convai_bridge.c) ================= */

/** Playback PCM ring (500ms @ 8kHz mono 16bit). */
#define CONVAI_PLAYBACK_RING_BYTES   8000
/** Prime threshold before starting playback HW (160ms jitter cover). */
#define CONVAI_PLAYBACK_PRIME_BYTES  480
/** Record thread chunk (40ms @ 8kHz stereo interleaved 16bit). */
#define CONVAI_RECORD_BUF_BYTES      640
/** Playback drain chunk. */
#define CONVAI_PLAYBACK_BUF_BYTES    1024
/** JSON message copy buffer for on_message delivery. */
#define CONVAI_JSON_COPY_BYTES       2048
/** Startup config storage. */
#define CONVAI_STARTUP_CONFIG_BYTES  2048
/** Bridge task stacks (bytes). */
#define CONVAI_RECORD_TASK_STACK     6144
#define CONVAI_PLAYBACK_TASK_STACK   6144

/* ================= AItalk app core (不计入 SDK 层预算, 仅信息列出) ======== */

#define CONVAI_AITALK_MSG_NUM     4
#define CONVAI_AITALK_MSG_LEN     512
#define CONVAI_AITALK_UI_STACK    4096

/* ================= budget roll-up ================= */

/** 开源引擎 + convai.v1 协议栈 + WS 传输。 */
#define CONVAI_BUDGET_SDK_BYTES \
    (CONVAI_ENC_BUF_BYTES + CONVAI_DEC_PCM_SAMPLES * 2 + \
     CONVAI_RX_RING_BYTES + CONVAI_TX_FRAMES * (CONVAI_TX_SLOT + 2) + \
     CONVAI_DEC_PCM_SAMPLES * 2 /* pump msg buf */ + \
     CONVAI_WS_RX_TASK_STACK + CONVAI_PUMP_TASK_STACK + CONVAI_SEND_TASK_STACK + \
     CONVAI_WSC_RX_BYTES + CONVAI_WSC_TX_BYTES + 768 /* engine struct misc */)

/** convai_bridge 集成层。 */
#define CONVAI_BUDGET_BRIDGE_BYTES \
    (CONVAI_PLAYBACK_RING_BYTES + CONVAI_RECORD_BUF_BYTES + \
     CONVAI_RECORD_BUF_BYTES / 2 /* mono buf */ + \
     CONVAI_PLAYBACK_BUF_BYTES + CONVAI_JSON_COPY_BYTES + \
     CONVAI_STARTUP_CONFIG_BYTES + \
     CONVAI_RECORD_TASK_STACK + CONVAI_PLAYBACK_TASK_STACK)

/** cJSON transient working set allowance. */
#define CONVAI_BUDGET_JSON_TRANSIENT  2048

/** WSS 增量（TLS 记录缓冲 + 握手峰值）。 */
#define CONVAI_BUDGET_TLS_BYTES \
    (CONVAI_TLS_IN_CONTENT_LEN + CONVAI_TLS_OUT_CONTENT_LEN + \
     CONVAI_TLS_HANDSHAKE_BYTES)

/** SDK 层总预算（预算口径 = 引擎+协议栈+传输+bridge，不含应用本体）。 */
#define CONVAI_BUDGET_SDKLAYER_WS_BYTES \
    (CONVAI_BUDGET_SDK_BYTES + CONVAI_BUDGET_BRIDGE_BYTES + \
     CONVAI_BUDGET_JSON_TRANSIENT)

#define CONVAI_BUDGET_SDKLAYER_WSS_BYTES \
    (CONVAI_BUDGET_SDKLAYER_WS_BYTES + CONVAI_BUDGET_TLS_BYTES)

/** 应用本体（信息列出，不纳入 100KB 断言）。 */
#define CONVAI_BUDGET_APP_BYTES \
    (CONVAI_AITALK_MSG_NUM * CONVAI_AITALK_MSG_LEN + 64 + CONVAI_AITALK_UI_STACK)

#define CONVAI_BUDGET_LIMIT_BYTES  102400   /* 100 KB */

#if defined(__cplusplus)
#define CONVAI_STATIC_ASSERT static_assert
#else
#define CONVAI_STATIC_ASSERT _Static_assert
#endif

CONVAI_STATIC_ASSERT(CONVAI_BUDGET_SDKLAYER_WS_BYTES <= CONVAI_BUDGET_LIMIT_BYTES,
                     "convai SDK-layer (ws) memory budget exceeds 100KB");

CONVAI_STATIC_ASSERT(CONVAI_BUDGET_SDKLAYER_WSS_BYTES <= CONVAI_BUDGET_LIMIT_BYTES,
                     "convai SDK-layer (wss) memory budget exceeds 100KB");

#endif /* CONVAI_LIMITS_H */
