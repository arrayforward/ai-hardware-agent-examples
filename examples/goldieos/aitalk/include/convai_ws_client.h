/**
 * @file convai_ws_client.h
 * @brief Minimal blocking WebSocket client (RFC 6455) for WS63.
 *
 * Plain TCP via mbedtls_net (lwIP). Client-side masking per RFC.
 * Single-threaded: one caller sends, one recv thread reads.
 * No heap allocation after connect (static buffers).
 */
#ifndef CONVAI_WS_CLIENT_H
#define CONVAI_WS_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "convai_limits.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONVAI_WSC_RX_BUF   CONVAI_WSC_RX_BYTES   /* max single incoming frame payload */
#define CONVAI_WSC_TX_BUF   CONVAI_WSC_TX_BYTES   /* max single outgoing frame payload */

typedef struct convai_wsc_s convai_wsc_t;

typedef enum {
    CONVAI_WSC_EV_TEXT = 1,     /* complete text frame received */
    CONVAI_WSC_EV_BINARY = 2,   /* complete binary frame received */
    CONVAI_WSC_EV_CLOSED = 3,   /* server closed / connection lost */
} convai_wsc_event_e;

/** Called from convai_wsc_poll(); payload valid only during the call. */
typedef void (*convai_wsc_cb)(convai_wsc_event_e ev,
                              const uint8_t *payload, size_t len, void *ud);

/**
 * TCP connect + WS opening handshake (with sub-protocol).
 * @param host, port, path (e.g. "/"), subprotocol (e.g. "convai.v1", may be NULL)
 * @return handle, or NULL on failure
 */
convai_wsc_t *convai_wsc_connect(const char *host, uint16_t port,
                                 const char *path, const char *subprotocol);

/** Send a text frame (payload copied into static tx buffer; len <= TX_BUF-14). */
int convai_wsc_send_text(convai_wsc_t *c, const char *data, size_t len);

/** Send a binary frame. */
int convai_wsc_send_bin(convai_wsc_t *c, const uint8_t *data, size_t len);

/** Send a WebSocket close frame. */
int convai_wsc_close(convai_wsc_t *c);

/**
 * Read and dispatch exactly one frame (blocking).
 * Handles ping->pong automatically. Returns:
 *   1 = frame dispatched, 0 = would block/timeout (see timeout_ms), -1 = closed
 */
int convai_wsc_poll(convai_wsc_t *c, convai_wsc_cb cb, void *ud, int timeout_ms);

/** Free resources and close the socket. */
void convai_wsc_destroy(convai_wsc_t *c);

/** Non-zero when the TCP connection is alive. */
int convai_wsc_is_connected(convai_wsc_t *c);

#ifdef __cplusplus
}
#endif

#endif /* CONVAI_WS_CLIENT_H */
