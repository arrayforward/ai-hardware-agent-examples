/**
 * @file net_sockets_host.c
 * @brief WinSock2 implementation of the mbedtls_net shim (host E2E only).
 *
 * A 100ms SO_RCVTIMEO makes recv() return periodically so that
 * convai_ws_client's read_exact() timeout/cancellation logic works.
 */
#include "mbedtls/net_sockets.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <string.h>

static void wsa_ensure_init(void)
{
    static int done = 0;
    if (!done) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        done = 1;
    }
}

void mbedtls_net_init(mbedtls_net_context *ctx)
{
    if (ctx) ctx->fd = -1;
}

int mbedtls_net_connect(mbedtls_net_context *ctx, const char *host,
                        const char *port, int proto)
{
    (void)proto;
    wsa_ensure_init();

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0) return -1;

    int fd = (int)socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    if (connect(fd, res->ai_addr, (int)res->ai_addrlen) != 0) {
        closesocket(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);

    DWORD tv = 100;   /* ms — periodic wakeup for poll timeouts */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));

    ctx->fd = fd;
    return 0;
}

int mbedtls_net_send(void *ctx, const unsigned char *buf, size_t len)
{
    mbedtls_net_context *c = ctx;
    int ret = send(c->fd, (const char *)buf, (int)len, 0);
    if (ret == SOCKET_ERROR) return -1;
    return ret;
}

int mbedtls_net_recv(void *ctx, unsigned char *buf, size_t len)
{
    mbedtls_net_context *c = ctx;
    int ret = recv(c->fd, (char *)buf, (int)len, 0);
    if (ret == 0) return MBEDTLS_ERR_NET_CONN_RESET;
    if (ret == SOCKET_ERROR) {
        int e = WSAGetLastError();
        if (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_READ;
        if (e == WSAECONNRESET || e == WSAECONNABORTED) return MBEDTLS_ERR_NET_CONN_RESET;
        return -1;
    }
    return ret;
}

void mbedtls_net_free(mbedtls_net_context *ctx)
{
    if (ctx && ctx->fd >= 0) {
        closesocket(ctx->fd);
        ctx->fd = -1;
    }
}
