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

#include "goldie_osal.h"
#include "convai_ws_client.h"

struct convai_wsc_s {
    mbedtls_net_context net;
    uint8_t *rx;        /* CONVAI_WSC_RX_BUF */
    uint8_t *tx;        /* CONVAI_WSC_TX_BUF */
    int      connected;
};

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

/* ---------- low-level IO ---------- */

static int write_all(convai_wsc_t *c, const uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        int ret = mbedtls_net_send(&c->net, buf + off, len - off);
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
        int ret = mbedtls_net_recv(&c->net, buf + off, len - off);
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

/* ---------- handshake ---------- */

convai_wsc_t *convai_wsc_connect(const char *host, uint16_t port,
                                 const char *path, const char *subprotocol)
{
    if (!host || !path) return NULL;

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
    if (c->rx) goldie_free(c->rx);
    if (c->tx) goldie_free(c->tx);
    mbedtls_net_free(&c->net);
    goldie_free(c);
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
    mbedtls_net_free(&c->net);
    if (c->rx) goldie_free(c->rx);
    if (c->tx) goldie_free(c->tx);
    goldie_free(c);
}

int convai_wsc_is_connected(convai_wsc_t *c)
{
    return c && c->connected;
}
