/**
 * @file convai_root_ca.c
 * @brief Embedded root CA for WSS server verification (convai_open engine).
 *
 * TEST CERTIFICATE — 本地测试网关专用 (ECDSA P-256, CN=router.local,
 * SAN: router.local/localhost/127.0.0.1, basicConstraints CA:TRUE)。
 * 生产部署：用 router 仓库 scripts/gen_cert.sh <你的域名或IP> 重新生成证书
 * (注意必须带 basicConstraints=critical,CA:TRUE, 否则 mbedTLS 验链报
 * BADCERT_NOT_TRUSTED), 用新的 server_ca.pem 内容替换下面的字符串,
 * 并把 server.key 配到网关侧 (切勿把生产私钥提交进仓库)。
 */
const char g_convai_root_ca_pem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBvzCCAWWgAwIBAgIUHyd+5VhozycGvupbd8jIEO4B1bowCgYIKoZIzj0EAwIw\n"
    "FzEVMBMGA1UEAwwMcm91dGVyLmxvY2FsMB4XDTI2MDczMTAwNTA1MloXDTM2MDcy\n"
    "ODAwNTA1MlowFzEVMBMGA1UEAwwMcm91dGVyLmxvY2FsMFkwEwYHKoZIzj0CAQYI\n"
    "KoZIzj0DAQcDQgAEhSpKDYjVSH2pyYhS920qwc1n4VBM9Pe1dSHL9S3rUJEGLqVK\n"
    "VJ6tlClKU0itQ1MHkOvWuALGhY/9rer2xxqOeaOBjjCBizAdBgNVHQ4EFgQU0h7A\n"
    "OAmVl9ydSGT0rpBopDWTKSIwHwYDVR0jBBgwFoAU0h7AOAmVl9ydSGT0rpBopDWT\n"
    "KSIwKAYDVR0RBCEwH4IMcm91dGVyLmxvY2Fsgglsb2NhbGhvc3SHBH8AAAEwDwYD\n"
    "VR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMCAoQwCgYIKoZIzj0EAwIDSAAwRQIh\n"
    "AM8FUgxHugNgHmixv9Ql//1GFOQU39xCv6ryTIygOORFAiABsxgUi7b4jdnimKA9\n"
    "6Lp/o6cZy8DXc+r/W+r6ae9+tA==\n"
    "-----END CERTIFICATE-----\n";
