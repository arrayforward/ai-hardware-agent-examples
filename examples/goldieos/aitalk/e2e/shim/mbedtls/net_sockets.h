/**
 * @file net_sockets.h (host shim: mbedtls_net over POSIX sockets)
 *        NOT used in the WS63 firmware build (real mbedtls is used there).
 */
#ifndef MBEDTLS_NET_SOCKETS_H
#define MBEDTLS_NET_SOCKETS_H

#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MBEDTLS_NET_PROTO_TCP 0
#define MBEDTLS_ERR_NET_CONN_RESET (-0x0050)

typedef struct mbedtls_net_context { int fd; } mbedtls_net_context;

static inline void mbedtls_net_init(mbedtls_net_context *ctx) { ctx->fd = -1; }

static inline int mbedtls_net_connect(mbedtls_net_context *ctx, const char *host,
                                      const char *port, int proto)
{
    (void)proto;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) return -1;
    ctx->fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    int rc = -1;
    if (ctx->fd >= 0) {
        rc = connect(ctx->fd, res->ai_addr, res->ai_addrlen);
        if (rc != 0) { close(ctx->fd); ctx->fd = -1; }
    }
    freeaddrinfo(res);
    return rc;
}

static inline int mbedtls_net_send(mbedtls_net_context *ctx, const unsigned char *buf, size_t len)
{
    return (int)send(ctx->fd, buf, len, MSG_NOSIGNAL);
}

static inline int mbedtls_net_recv(mbedtls_net_context *ctx, unsigned char *buf, size_t len)
{
    int rc = (int)recv(ctx->fd, buf, len, 0);
    if (rc == 0) return MBEDTLS_ERR_NET_CONN_RESET;
    if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return rc;
}

static inline void mbedtls_net_free(mbedtls_net_context *ctx)
{
    if (ctx->fd >= 0) close(ctx->fd);
    ctx->fd = -1;
}

#ifdef __cplusplus
}
#endif

#endif /* MBEDTLS_NET_SOCKETS_H */
