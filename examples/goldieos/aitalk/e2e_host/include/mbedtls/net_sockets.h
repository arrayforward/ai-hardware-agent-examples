/**
 * @file net_sockets.h
 * @brief Host (Windows/WinSock) shim of the mbedtls_net API, just enough
 *        for aitalk/src/convai_ws_client.c to compile and run unmodified
 *        in the host-side E2E harness. NOT used on WS63 (real mbedtls).
 */
#ifndef MBEDTLS_NET_SOCKETS_H
#define MBEDTLS_NET_SOCKETS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MBEDTLS_NET_PROTO_TCP   0
#define MBEDTLS_ERR_NET_CONN_RESET  (-0x0050)

typedef struct {
    int fd;
} mbedtls_net_context;

void mbedtls_net_init(mbedtls_net_context *ctx);
int  mbedtls_net_connect(mbedtls_net_context *ctx, const char *host,
                         const char *port, int proto);
int  mbedtls_net_send(void *ctx, const unsigned char *buf, size_t len);
int  mbedtls_net_recv(void *ctx, unsigned char *buf, size_t len);
void mbedtls_net_free(mbedtls_net_context *ctx);

#ifdef __cplusplus
}
#endif

#endif /* MBEDTLS_NET_SOCKETS_H */
