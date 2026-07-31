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
 * TLS (WSS) configuration for convai_wsc_connect_ex().
 *
 * Security design (与网关 scripts/gen_cert.sh 配套, 见 convai_limits.h §TLS):
 *  - TLS 1.2 only, 套件限定 ECDHE-ECDSA/RSA + AES-256/128-GCM
 *    (AES-256-GCM 优先; WS63 有 AES 硬件加速, 避免纯软件 ChaCha20)
 *  - 证书 ECDSA P-256 (RISC-V/Xtensa 无大数指令, ECDSA 握手比 RSA-2048 快约 10 倍)
 */
typedef struct {
    const char *ca_pem;      /* PEM CA (NUL 结尾), 用于验签; NULL=不验签(不安全, 仅调试) */
    int         skip_cn_check; /* 1 = IP 直连时跳过 CN/SAN 主机名校验 */
    const int  *ciphersuites;  /* mbedTLS 套件 id 列表 (0 结尾) 覆盖默认白名单;
                                  NULL = 默认 (AES-256-GCM 优先, AES-128-GCM 兜底)。
                                  强制 AES-256 时传 {ECDSA_256_GCM_SHA384,
                                  RSA_256_GCM_SHA384, 0} */
} convai_wsc_tls_t;

/**
 * TCP connect + WS opening handshake (with sub-protocol).
 * @param host, port, path (e.g. "/"), subprotocol (e.g. "convai.v1", may be NULL)
 * @return handle, or NULL on failure
 */
convai_wsc_t *convai_wsc_connect(const char *host, uint16_t port,
                                 const char *path, const char *subprotocol);

/**
 * Same as convai_wsc_connect(), with optional TLS (WSS).
 * @param tls  NULL = plain ws; non-NULL = TLS with the given CA/校验选项
 */
convai_wsc_t *convai_wsc_connect_ex(const char *host, uint16_t port,
                                    const char *path, const char *subprotocol,
                                    const convai_wsc_tls_t *tls);

/** Negotiated TLS ciphersuite name (e.g. "TLS-ECDHE-ECDSA-WITH-AES-256-GCM-SHA384"),
 *  or NULL when not using TLS. */
const char *convai_wsc_get_ciphersuite(convai_wsc_t *c);

/** AES-256-GCM-only suite list (0 结尾), 用于 convai_wsc_tls_t.ciphersuites
 *  强制 AES-256 (Go 网关偏好 AES-128, 默认宽名单会协商成 128)。 */
extern const int g_convai_wsc_suites_aes256[];

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
