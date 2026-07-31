/**
 * @file convai_ws_client.c
 * @brief Minimal blocking WebSocket client (RFC 6455) for WS63.
 *
 * Transport: mbedtls_net_context (plain TCP over lwIP).
 * Memory: rx/tx buffers are per-instance (allocated once at connect).
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "mbedtls/net_sockets.h"
#if !defined(CONVAI_WSC_NO_TLS)
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#define CONVAI_WSC_TLS  1
#endif

#include "goldie_osal.h"
#include "convai_ws_client.h"

struct convai_wsc_s {
    mbedtls_net_context net;
    uint8_t *rx;        /* CONVAI_WSC_RX_BUF */
    uint8_t *tx;        /* CONVAI_WSC_TX_BUF */
    int      connected;
#if CONVAI_WSC_TLS
    int      tls_active;     /* handshake done, IO goes through ssl */
    int      tls_inited;     /* mbedtls contexts initialised (need teardown) */
    mbedtls_ssl_context        ssl;
    mbedtls_ssl_config         conf;
    mbedtls_entropy_context    entropy;
    mbedtls_ctr_drbg_context   drbg;
    mbedtls_x509_crt           ca;
#endif
};

#if CONVAI_WSC_TLS
/*
 * MCU 友好套件白名单 (与网关 runTLS 完全一致):
 * AES-256-GCM 优先 (WS63 AES 硬件加速), ECDHE-ECDSA 优先于 ECDHE-RSA
 * (P-256 标量乘远比 RSA-2048 验签便宜)。TLS 1.2, 握手只发生一次 (长连接)。
 */
static const int s_tls_suites[] = {
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    0
};

/** 强制 AES-256-GCM 白名单 (Go 网关偏好 AES-128, 需要 256 时必须收窄 offers) */
const int g_convai_wsc_suites_aes256[] = {
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
    0
};
#endif

/* ---------- base64 (for Sec-WebSocket-Key) ---------- */

static const char s_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap)
{
    size_t need = ((in_len + 2) / 3) * 4 + 1;
    if (out_cap < need) return -1;
    size_t o = 0;
    for (size_t i = 0; i < in_len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        int rem = (int)(in_len - i);
        if (rem > 1) v |= (uint32_t)in[i + 1] << 8;
        if (rem > 2) v |= in[i + 2];
        out[o++] = s_b64[(v >> 18) & 0x3F];
        out[o++] = s_b64[(v >> 12) & 0x3F];
        out[o++] = rem > 1 ? s_b64[(v >> 6) & 0x3F] : '=';
        out[o++] = rem > 2 ? s_b64[v & 0x3F] : '=';
    }
    out[o] = 0;
    return (int)o;
}

/* ---------- low-level IO (plain or TLS, selected at connect) ---------- */

static int low_send(convai_wsc_t *c, const uint8_t *buf, size_t len)
{
#if CONVAI_WSC_TLS
    if (c->tls_active) {
        int ret;
        do {
            ret = mbedtls_ssl_write(&c->ssl, buf, len);
        } while (ret == MBEDTLS_ERR_SSL_WANT_WRITE);
        return ret;
    }
#endif
    return mbedtls_net_send(&c->net, buf, len);
}

static int low_recv(convai_wsc_t *c, uint8_t *buf, size_t len)
{
#if CONVAI_WSC_TLS
    if (c->tls_active) {
        int ret = mbedtls_ssl_read(&c->ssl, buf, len);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ) return -2;   /* would block */
        if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
            return MBEDTLS_ERR_NET_CONN_RESET;
        return ret;
    }
#endif
    return mbedtls_net_recv(&c->net, buf, len);
}

static int write_all(convai_wsc_t *c, const uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        int ret = low_send(c, buf + off, len - off);
        if (ret <= 0) return -1;
        off += (size_t)ret;
    }
    return 0;
}

/* read exactly len bytes; returns 1 ok, 0 timeout, -1 error/closed */
static int read_exact(convai_wsc_t *c, uint8_t *buf, size_t len, int timeout_ms)
{
    size_t off = 0;
    int waited = 0;
    while (off < len) {
        int ret = low_recv(c, buf + off, len - off);
        if (ret > 0) {
            off += (size_t)ret;
            continue;
        }
        if (ret == 0 || ret == MBEDTLS_ERR_NET_CONN_RESET) return -1;
        /* MBEDTLS_ERR_SSL_WANT_READ / timeout on blocking socket: small sleep */
        if (timeout_ms >= 0 && waited >= timeout_ms) return 0;
        goldie_msleep(2);
        waited += 2;
    }
    return 1;
}

/* ---------- TLS ---------- */

#if CONVAI_WSC_TLS
static int tls_handshake(convai_wsc_t *c, const char *host,
                         const convai_wsc_tls_t *tls)
{
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_entropy_init(&c->entropy);
    mbedtls_ctr_drbg_init(&c->drbg);
    mbedtls_x509_crt_init(&c->ca);
    c->tls_inited = 1;

    if (mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->entropy,
                              (const uint8_t *)"convai_ws63", 11) != 0) {
        return -1;
    }
    if (mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        return -1;
    }
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);
    mbedtls_ssl_conf_ciphersuites(&c->conf,
                                  tls->ciphersuites ? tls->ciphersuites
                                                    : s_tls_suites);
    mbedtls_ssl_conf_min_version(&c->conf, MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3);   /* TLS 1.2 */
    mbedtls_ssl_conf_max_version(&c->conf, MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3);

    if (tls->ca_pem) {
        int prc = mbedtls_x509_crt_parse(&c->ca, (const uint8_t *)tls->ca_pem,
                                         strlen(tls->ca_pem) + 1);
        if (prc != 0) {
            printf("[convai_wsc] CA parse failed: -0x%04x\n", -prc);
            return -1;
        }
        mbedtls_ssl_conf_ca_chain(&c->conf, &c->ca, NULL);
        mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    } else {
        /* 无 CA: 不验签 (不安全, 仅调试) */
        mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_NONE);
    }

    if (mbedtls_ssl_setup(&c->ssl, &c->conf) != 0) return -1;
    if (tls->ca_pem && !tls->skip_cn_check) {
        if (mbedtls_ssl_set_hostname(&c->ssl, host) != 0) return -1;
    }
    mbedtls_ssl_set_bio(&c->ssl, &c->net, mbedtls_net_send, mbedtls_net_recv, NULL);

    int ret;
    while ((ret = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            printf("[convai_wsc] TLS handshake failed: -0x%04x (verify=0x%08x)\n",
                   -ret, (unsigned)mbedtls_ssl_get_verify_result(&c->ssl));
            return -1;
        }
    }
    c->tls_active = 1;
    return 0;
}

static void tls_teardown(convai_wsc_t *c)
{
    if (!c->tls_inited) return;
    if (c->tls_active) mbedtls_ssl_close_notify(&c->ssl);
    mbedtls_x509_crt_free(&c->ca);
    mbedtls_ctr_drbg_free(&c->drbg);
    mbedtls_entropy_free(&c->entropy);
    mbedtls_ssl_config_free(&c->conf);
    mbedtls_ssl_free(&c->ssl);
    c->tls_active = 0;
    c->tls_inited = 0;
}
#endif /* CONVAI_WSC_TLS */

/* ---------- handshake ---------- */

convai_wsc_t *convai_wsc_connect_ex(const char *host, uint16_t port,
                                    const char *path, const char *subprotocol,
                                    const convai_wsc_tls_t *tls)
{
    if (!host || !path) return NULL;
#if !CONVAI_WSC_TLS
    if (tls) return NULL;              /* TLS support compiled out */
#endif

    convai_wsc_t *c = goldie_malloc(sizeof(*c));
    if (!c) return NULL;
    memset(c, 0, sizeof(*c));
    mbedtls_net_init(&c->net);

    c->rx = goldie_malloc(CONVAI_WSC_RX_BUF);
    c->tx = goldie_malloc(CONVAI_WSC_TX_BUF);
    if (!c->rx || !c->tx) goto fail;

    {
        char svc[8];
        snprintf(svc, sizeof(svc), "%u", port);
        if (mbedtls_net_connect(&c->net, host, svc, MBEDTLS_NET_PROTO_TCP) != 0) {
            goto fail;
        }
    }

#if CONVAI_WSC_TLS
    if (tls && tls_handshake(c, host, tls) != 0) goto fail;
#endif

    /* opening handshake */
    {
        uint8_t key_raw[16];
        char key_b64[32];
        for (int i = 0; i < 16; i++) key_raw[i] = (uint8_t)(rand() & 0xFF);
        b64_encode(key_raw, 16, key_b64, sizeof(key_b64));

        int n = snprintf((char *)c->tx, CONVAI_WSC_TX_BUF,
                         "GET %s HTTP/1.1\r\n"
                         "Host: %s:%u\r\n"
                         "Upgrade: websocket\r\n"
                         "Connection: Upgrade\r\n"
                         "Sec-WebSocket-Key: %s\r\n"
                         "Sec-WebSocket-Version: 13\r\n"
                         "%s%s\r\n"
                         "\r\n",
                         path, host, port, key_b64,
                         subprotocol ? "Sec-WebSocket-Protocol: " : "",
                         subprotocol ? subprotocol : "");
        if (n <= 0 || n >= CONVAI_WSC_TX_BUF) goto fail;
        if (write_all(c, c->tx, (size_t)n) != 0) goto fail;

        /* read response headers (up to rx buf, expect "101") */
        int rc = read_exact(c, c->rx, 12, 3000);   /* "HTTP/1.1 101" */
        if (rc != 1 || memcmp(c->rx, "HTTP/1.1 101", 12) != 0) goto fail;
        /* drain headers until \r\n\r\n */
        size_t matched = 0;
        while (matched < 4) {
            uint8_t b;
            rc = read_exact(c, &b, 1, 3000);
            if (rc != 1) goto fail;
            if ((matched % 2 == 0 && b == '\r') || (matched % 2 == 1 && b == '\n')) {
                matched++;
            } else {
                matched = (b == '\r') ? 1 : 0;
            }
        }
    }

    c->connected = 1;
    return c;

fail:
#if CONVAI_WSC_TLS
    tls_teardown(c);
#endif
    if (c->rx) goldie_free(c->rx);
    if (c->tx) goldie_free(c->tx);
    mbedtls_net_free(&c->net);
    goldie_free(c);
    return NULL;
}

convai_wsc_t *convai_wsc_connect(const char *host, uint16_t port,
                                 const char *path, const char *subprotocol)
{
    return convai_wsc_connect_ex(host, port, path, subprotocol, NULL);
}

const char *convai_wsc_get_ciphersuite(convai_wsc_t *c)
{
#if CONVAI_WSC_TLS
    if (c && c->tls_active) return mbedtls_ssl_get_ciphersuite(&c->ssl);
#endif
    (void)c;
    return NULL;
}

/* ---------- frame send ---------- */

static int send_frame(convai_wsc_t *c, uint8_t opcode, const uint8_t *data, size_t len)
{
    if (!c->connected) return -1;
    if (len + 14 > CONVAI_WSC_TX_BUF) return -1;   /* hdr(<=10)+mask(4) */

    size_t h = 0;
    c->tx[h++] = 0x80 | opcode;
    if (len < 126) {
        c->tx[h++] = 0x80 | (uint8_t)len;           /* MASK bit set */
    } else if (len <= 0xFFFF) {
        c->tx[h++] = 0x80 | 126;
        c->tx[h++] = (uint8_t)(len >> 8);
        c->tx[h++] = (uint8_t)(len & 0xFF);
    } else {
        return -1;                                   /* >64KB not needed */
    }
    uint8_t mask[4];
    for (int i = 0; i < 4; i++) mask[i] = (uint8_t)(rand() & 0xFF);
    memcpy(c->tx + h, mask, 4);
    h += 4;
    for (size_t i = 0; i < len; i++) {
        c->tx[h + i] = data[i] ^ mask[i % 4];
    }
    return write_all(c, c->tx, h + len);
}

int convai_wsc_send_text(convai_wsc_t *c, const char *data, size_t len)
{
    return send_frame(c, 0x1, (const uint8_t *)data, len);
}

int convai_wsc_send_bin(convai_wsc_t *c, const uint8_t *data, size_t len)
{
    return send_frame(c, 0x2, data, len);
}

int convai_wsc_close(convai_wsc_t *c)
{
    if (!c || !c->connected) return 0;
    int rc = send_frame(c, 0x8, NULL, 0);
    c->connected = 0;
    return rc;
}

/* ---------- frame recv ---------- */

int convai_wsc_poll(convai_wsc_t *c, convai_wsc_cb cb, void *ud, int timeout_ms)
{
    if (!c->connected) return -1;

    uint8_t hdr[2];
    int rc = read_exact(c, hdr, 2, timeout_ms);
    if (rc <= 0) return rc;                        /* 0 timeout, -1 closed */

    /* uint8_t fin = hdr[0] >> 7; */
    uint8_t opcode = hdr[0] & 0x0F;
    int masked = hdr[1] >> 7;
    uint64_t len = hdr[1] & 0x7F;

    if (len == 126) {
        uint8_t ext[2];
        if (read_exact(c, ext, 2, timeout_ms) != 1) return -1;
        len = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (len == 127) {
        uint8_t ext[8];
        if (read_exact(c, ext, 8, timeout_ms) != 1) return -1;
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | ext[i];
    }
    if (len > CONVAI_WSC_RX_BUF) return -1;        /* oversize: bail out */

    uint8_t mask[4] = {0};
    if (masked && read_exact(c, mask, 4, timeout_ms) != 1) return -1;

    if (len > 0 && read_exact(c, c->rx, (size_t)len, timeout_ms) != 1) return -1;
    if (masked) {
        for (uint64_t i = 0; i < len; i++) c->rx[i] ^= mask[i % 4];
    }

    switch (opcode) {
    case 0x1:
        if (cb) cb(CONVAI_WSC_EV_TEXT, c->rx, (size_t)len, ud);
        return 1;
    case 0x2:
        if (cb) cb(CONVAI_WSC_EV_BINARY, c->rx, (size_t)len, ud);
        return 1;
    case 0x8:                                      /* close */
        c->connected = 0;
        send_frame(c, 0x8, NULL, 0);
        if (cb) cb(CONVAI_WSC_EV_CLOSED, NULL, 0, ud);
        return -1;
    case 0x9:                                      /* ping -> pong */
        send_frame(c, 0xA, c->rx, (size_t)len);
        return 1;
    case 0xA:                                      /* pong */
        return 1;
    default:                                       /* continuation/unknown: skip */
        return 1;
    }
}

void convai_wsc_destroy(convai_wsc_t *c)
{
    if (!c) return;
    if (c->connected) convai_wsc_close(c);
#if CONVAI_WSC_TLS
    tls_teardown(c);
#endif
    mbedtls_net_free(&c->net);
    if (c->rx) goldie_free(c->rx);
    if (c->tx) goldie_free(c->tx);
    goldie_free(c);
}

int convai_wsc_is_connected(convai_wsc_t *c)
{
    return c && c->connected;
}
