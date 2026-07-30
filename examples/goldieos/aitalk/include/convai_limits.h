/**
 * @file convai_limits.h
 * @brief Single source of truth for the AItalk convai sub-system memory
 *        budget on WS63 goldieos (target: protocol stack + open SDK +
 *        bridge + app core < 100 KB).
 *
 * 参考 05-memory-optimization-100kb.md 的路线:
 *   路线1 消除每帧 malloc (静态工作缓冲)
 *   路线3 静态池 + 丢帧不崩溃 (TX 队列丢最老 / RX 环整帧丢弃)
 *   路线5 可观测性 (convai_open_mem_report)
 *
 * All buffers/tasks of the convai sub-system are sized HERE and only here.
 * The compile-time assertion at the bottom fails the build when the
 * budget exceeds 100 KB (102400 bytes), so the budget is self-enforcing.
 *
 * Budget table (steady state, plain ws, G.711A codec — stateless):
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
 *   | engine 结构体其余字段                       | ~700   | convai_open.c           |
 *   | SDK 小计                                   | ~46.3K |                         |
 *   | 播放环 g_ring_data                         | 8000   | convai_bridge.c         |
 *   | 录音静态缓冲 3x640B                         | 1920   | convai_bridge.c         |
 *   | 播放静态缓冲                                | 1024   | convai_bridge.c         |
 *   | JSON 拷贝缓冲                               | 2048   | convai_bridge.c         |
 *   | startup config 缓冲                        | 2048   | convai_bridge.c         |
 *   | 任务栈 record 6K + playback 6K             | 12288  | convai_bridge.c         |
 *   | bridge 小计                                | ~26.7K |                         |
 *   | AItalk 应用核心 (4x512 + 状态)             | 2112   | aitalk_core.c           |
 *   | AItalk UI 线程栈                           | 4096   | apps/AItalk             |
 *   | cJSON 瞬态工作集                            | ~2048  | cJSON (heap, 瞬态)      |
 *   |-------------------------------------------|--------|-------------------------|
 *   | 合计                                       | ~82.2K | < 102400 OK             |
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
#define CONVAI_WS_RX_TASK_STACK   8192   /* recv thread: JSON parse */
#define CONVAI_PUMP_TASK_STACK    4096   /* downlink pump thread */
#define CONVAI_SEND_TASK_STACK    4096   /* uplink send thread */

/** WebSocket client buffers (allocated once per connect). */
#define CONVAI_WSC_RX_BYTES       4096
#define CONVAI_WSC_TX_BYTES       1536

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

/* ================= AItalk app core (aitalk_core.c) ================= */

#define CONVAI_AITALK_MSG_NUM     4
#define CONVAI_AITALK_MSG_LEN     512
#define CONVAI_AITALK_UI_STACK    4096

/* ================= budget roll-up ================= */

#define CONVAI_BUDGET_SDK_BYTES \
    (CONVAI_ENC_BUF_BYTES + CONVAI_DEC_PCM_SAMPLES * 2 + \
     CONVAI_RX_RING_BYTES + CONVAI_TX_FRAMES * (CONVAI_TX_SLOT + 2) + \
     CONVAI_DEC_PCM_SAMPLES * 2 /* pump msg buf */ + \
     CONVAI_WS_RX_TASK_STACK + CONVAI_PUMP_TASK_STACK + CONVAI_SEND_TASK_STACK + \
     CONVAI_WSC_RX_BYTES + CONVAI_WSC_TX_BYTES + 768 /* engine struct misc */)

#define CONVAI_BUDGET_BRIDGE_BYTES \
    (CONVAI_PLAYBACK_RING_BYTES + 3 * CONVAI_RECORD_BUF_BYTES + \
     CONVAI_PLAYBACK_BUF_BYTES + CONVAI_JSON_COPY_BYTES + \
     CONVAI_STARTUP_CONFIG_BYTES + \
     CONVAI_RECORD_TASK_STACK + CONVAI_PLAYBACK_TASK_STACK)

#define CONVAI_BUDGET_APP_BYTES \
    (CONVAI_AITALK_MSG_NUM * CONVAI_AITALK_MSG_LEN + 64 + CONVAI_AITALK_UI_STACK)

/** cJSON transient working set allowance. */
#define CONVAI_BUDGET_JSON_TRANSIENT  2048

#define CONVAI_BUDGET_TOTAL_BYTES \
    (CONVAI_BUDGET_SDK_BYTES + CONVAI_BUDGET_BRIDGE_BYTES + \
     CONVAI_BUDGET_APP_BYTES + CONVAI_BUDGET_JSON_TRANSIENT)

#define CONVAI_BUDGET_LIMIT_BYTES  102400   /* 100 KB */

#if defined(__cplusplus)
#define CONVAI_STATIC_ASSERT static_assert
#else
#define CONVAI_STATIC_ASSERT _Static_assert
#endif

CONVAI_STATIC_ASSERT(CONVAI_BUDGET_TOTAL_BYTES <= CONVAI_BUDGET_LIMIT_BYTES,
                     "convai sub-system memory budget exceeds 100KB");

#endif /* CONVAI_LIMITS_H */
