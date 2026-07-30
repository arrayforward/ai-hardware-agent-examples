/**
 * @file convai_open.h
 * @brief Open-source re-implementation of the ConvAI device SDK for WS63.
 *
 * Exposes the SAME public API as the closed libconvai_sdk.a
 * (include/convai/convai_api.h) so goldieos apps and convai_bridge.c
 * work unmodified. Wire protocol: convai.v1 over plain WebSocket
 * (aitalk/src/convai_ws_client.c).
 *
 * Memory: static pools only (see docs/AITALK_OPTIMIZATION.md §budget).
 */
#ifndef CONVAI_OPEN_H
#define CONVAI_OPEN_H

#include "convai/convai_api.h"
#include "convai_limits.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- static pool sizing (single source of truth: convai_limits.h) ---- */
#define CONVAI_OPEN_TX_FRAMES      CONVAI_TX_FRAMES
#define CONVAI_OPEN_TX_SLOT        CONVAI_TX_SLOT
#define CONVAI_OPEN_RX_RING_BYTES  CONVAI_RX_RING_BYTES
#define CONVAI_OPEN_ENC_BUF        CONVAI_ENC_BUF_BYTES
#define CONVAI_OPEN_DEC_SAMPLES    CONVAI_DEC_PCM_SAMPLES
#define CONVAI_OPEN_WS_TASK_STACK  CONVAI_WS_RX_TASK_STACK
#define CONVAI_OPEN_PUMP_STACK     CONVAI_PUMP_TASK_STACK
#define CONVAI_OPEN_SEND_STACK     CONVAI_SEND_TASK_STACK

/** Print memory/watermark report (pools, drops, high-water) via printf. */
void convai_open_mem_report(void);

/** Switch audio codec at runtime (see convai_codec.h ids; default G711A). */
int convai_open_set_codec(int codec_id);

/** Current codec id, or -1 when engine not created. */
int convai_open_get_codec(void);

#ifdef __cplusplus
}
#endif

#endif /* CONVAI_OPEN_H */
