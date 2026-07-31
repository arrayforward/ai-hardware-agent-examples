/**
 * @file e2e_main.c
 * @brief Host-side E2E device simulator for the open ConvAI SDK.
 *
 * Compiles the SAME device sources (aitalk/src/*.c) against the real
 * convai.v1 gateway (D:\dev\router) + mock backends, and verifies:
 *
 *   self-tests: ring wrap/drop-oldest, g711a roundtrip, envelope build/parse
 *   e2e:        hello -> hello_ack -> listening
 *               uplink sine -> ASR is_final -> thinking
 *               -> text reply -> answering -> ~40 audio frames (0.8s)
 *               -> answer_finished -> listening
 *
 * Exit code 0 = all checks passed.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "goldie_osal.h"

#include "convai/convai_api.h"
#include "convai_open.h"
#include "convai_limits.h"
#include "convai_ring.h"
#include "convai_protocol.h"
#include "convai_codec.h"
#include "convai_codec_g711a.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  [OK] %s\n", msg); } \
    else { printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

/* ================= self tests ================= */

static void test_ring(void)
{
    printf("-- convai_ring --\n");
    static uint8_t arena[64];
    convai_ring_t r;
    convai_ring_init(&r, arena, sizeof(arena));

    uint8_t out[64]; uint16_t olen;

    /* wrap-around + drop-oldest-whole-message */
    for (int i = 0; i < 8; i++) {
        uint8_t m[10];
        memset(m, i, sizeof(m));
        CHECK(convai_ring_push(&r, m, sizeof(m)) == 0, "push msg");
    }
    CHECK(convai_ring_drops(&r) > 0, "full -> oldest whole msg evicted");

    int first = -1, cnt = 0;
    while (convai_ring_pop(&r, out, sizeof(out), &olen) == 0) {
        if (first < 0) first = out[0];
        if (olen != 10) { g_fail++; printf("  [FAIL] msg len corrupted\n"); break; }
        cnt++;
    }
    CHECK(cnt > 0 && cnt < 8, "popped surviving whole messages");
    CHECK(first > 0, "oldest evicted, sequence continuous");

    /* oversize message dropped, counted */
    uint8_t big[80];
    CHECK(convai_ring_push(&r, big, sizeof(big)) == -1, "oversize msg rejected");
    CHECK(convai_ring_high_water(&r) <= sizeof(arena), "high-water within arena");
}

static void test_g711a(void)
{
    printf("-- g711a codec --\n");
    /* silence vector: 0 -> 0xD5 (涓庣綉鍏虫煡琛ㄤ竴鑷? */
    int16_t pcm[4] = {0, 1000, -1000, 3000};
    uint8_t enc[4];
    size_t enc_len = 0;
    CHECK(convai_g711a_encode((const uint8_t *)pcm, sizeof(pcm), 1,
                              enc, sizeof(enc), &enc_len) == 0 && enc_len == 4,
          "encode 4 samples");
    CHECK(enc[0] == 0xD5, "silence encodes to 0xD5 (router test vector)");

    uint8_t dec[8];
    size_t dec_len = 0;
    CHECK(convai_g711a_decode(enc, enc_len, dec, sizeof(dec), &dec_len) == 0
          && dec_len == sizeof(pcm), "decode roundtrip length");
    int16_t s = (int16_t)(dec[0] | (dec[1] << 8));
    CHECK(s >= -8 && s <= 8, "silence decodes near 0");

    /* pluggable codec descriptor roundtrip */
    const convai_codec_t *c = convai_codec_get(CONVAI_CODEC_G711A);
    CHECK(c && !strcmp(c->name, "g711a") && c->sample_rate == 8000, "registry g711a");
    CHECK(convai_codec_by_name("g711a") == c, "by_name lookup");
}

static void test_protocol(void)
{
    printf("-- convai.v1 protocol --\n");
    char *env = convai_proto_build_envelope("hello", "{\"a\":1}", 42, 1234);
    CHECK(env != NULL, "build envelope");
    convai_envelope_t p;
    CHECK(convai_proto_parse_envelope(env, strlen(env), &p) == 0, "parse envelope");
    CHECK(!strcmp(p.type, "hello") && p.seq == 42 && p.ts == 1234, "type/seq/ts");
    CHECK(p.body != NULL, "body object present");
    convai_proto_envelope_free(&p);
    free(env);

    uint8_t hdr[CONVAI_AUDIO_HDR_LEN];
    convai_proto_audio_hdr_pack(hdr, CONVAI_AUDIO_OP_FRAME, 0x01020304, 0x1122334455667788ULL);
    uint8_t op; uint32_t seq; uint64_t ts;
    CHECK(convai_proto_audio_hdr_unpack(hdr, sizeof(hdr), &op, &seq, &ts) == 0
          && op == CONVAI_AUDIO_OP_FRAME && seq == 0x01020304
          && ts == 0x1122334455667788ULL, "audio hdr pack/unpack BE");

    CHECK(convai_proto_status_from_str("answering") == CONVAI_STATUS_ANSWERING,
          "status str mapping");
}

static void test_budget(void)
{
    printf("-- memory budget (convai_limits.h, SDK 层口径, 不含应用本体) --\n");
    printf("  engine+stack %5d B\n", CONVAI_BUDGET_SDK_BYTES);
    printf("  bridge       %5d B\n", CONVAI_BUDGET_BRIDGE_BYTES);
    printf("  json transient %4d B\n", CONVAI_BUDGET_JSON_TRANSIENT);
    printf("  TLS (wss)    %5d B\n", CONVAI_BUDGET_TLS_BYTES);
    printf("  SDK层 ws     %5d B / %d B\n", CONVAI_BUDGET_SDKLAYER_WS_BYTES,
           CONVAI_BUDGET_LIMIT_BYTES);
    printf("  SDK层 wss    %5d B / %d B\n", CONVAI_BUDGET_SDKLAYER_WSS_BYTES,
           CONVAI_BUDGET_LIMIT_BYTES);
    printf("  (参考) app本体 %4d B (不计入预算)\n", CONVAI_BUDGET_APP_BYTES);
    CHECK(CONVAI_BUDGET_SDKLAYER_WS_BYTES <= CONVAI_BUDGET_LIMIT_BYTES,
          "SDK-layer ws budget under 100KB");
    CHECK(CONVAI_BUDGET_SDKLAYER_WSS_BYTES <= CONVAI_BUDGET_LIMIT_BYTES,
          "SDK-layer wss budget under 100KB");
}

/* ================= e2e vs real gateway ================= */

static volatile int g_connected = 0;
static volatile unsigned long g_status_mask = 0;
static volatile unsigned long g_audio_frames = 0;
static volatile unsigned long g_audio_bytes = 0;
static volatile int g_text_msgs = 0;

static void on_event(convai_engine_t e, convai_event_t *ev, void *ud)
{
    (void)e; (void)ud;
    printf("[e2e] event %d %s\n", ev->code, ev->data.details ? ev->data.details : "");
    if (ev->code == CONVAI_EV_CONNECTED) g_connected = 1;
    if (ev->code == CONVAI_EV_DISCONNECTED) g_connected = 0;
}

static void on_status(convai_engine_t e, convai_status_e s, void *ud)
{
    (void)e; (void)ud;
    g_status_mask |= (1UL << s);
    printf("[e2e] status -> %d\n", s);
}

static void on_audio(convai_engine_t e, const void *data, size_t len,
                     const convai_audio_frame_info_t *info, void *ud)
{
    (void)e; (void)data; (void)ud;
    if (info && info->data_type != CONVAI_AUDIO_DATA_TYPE_PCM16) {
        printf("  [FAIL] downlink not PCM16 (type=%d)\n", info->data_type);
        g_fail++;
    }
    g_audio_frames++;
    g_audio_bytes += len;
}

static void on_message(convai_engine_t e, const void *data, size_t len,
                       const convai_message_info_t *info, void *ud)
{
    (void)e; (void)ud;
    if (!info || !info->is_binary) {
        g_text_msgs++;
        printf("[e2e] text msg: %.*s\n", (int)len, (const char *)data);
    }
}

#define STATUS_SEEN(st) ((g_status_mask & (1UL << (st))) != 0)

/* One full conversation round against the gateway at @url.
 * @expect_suite: 期望协商套件包含的子串 (如 "AES-256-GCM"), NULL=明文 ws。
 * @skip_cn:      device-side tls.skip_cn_check (IP 直连场景)。
 * @aes256_only:  device-side tls.aes256_only (强制 AES-256-GCM)。 */
static void e2e_round(const char *url, const char *expect_suite,
                      int skip_cn, int aes256_only)
{
    printf("-- e2e round: %s (skip_cn=%d aes256_only=%d) --\n",
           url, skip_cn, aes256_only);
    g_connected = 0;
    g_status_mask = 0;
    g_audio_frames = g_audio_bytes = 0;
    g_text_msgs = 0;

    char cfg[768];
    snprintf(cfg, sizeof(cfg),
        "{\"info\":{\"product_id\":\"pid\",\"product_key\":\"pk\","
        "\"product_secret\":\"ps\",\"device_name\":\"e2e-host\"},"
        "\"ws\":{\"url\":\"%s\",\"audio\":{\"codec\":1},"
        "\"tls\":{\"skip_cn_check\":%s,\"aes256_only\":%s}}}",
        url, skip_cn ? "true" : "false", aes256_only ? "true" : "false");

    convai_event_handler_t h;
    memset(&h, 0, sizeof(h));
    h.on_convai_event = on_event;
    h.on_convai_conversation_status = on_status;
    h.on_convai_audio_data = on_audio;
    h.on_convai_message_data = on_message;

    convai_engine_t eng = NULL;
    CHECK(convai_create(&eng, cfg, &h, NULL) == CONVAI_OK && eng, "convai_create");
    if (!eng) return;

    convai_opt_t opt = { .mode = CONVAI_MODE_WS, .agent_id = "e2e-agent",
                         .params = "{\"config\":{}}" };
    CHECK(convai_start(eng, &opt) == CONVAI_OK, "convai_start");

    if (expect_suite) {
        const char *cs = convai_open_get_ciphersuite();
        printf("[e2e] TLS ciphersuite: %s\n", cs ? cs : "(none)");
        char msg[96];
        snprintf(msg, sizeof(msg), "negotiated suite contains %s", expect_suite);
        CHECK(cs && strstr(cs, expect_suite) != NULL, msg);
    }

    /* wait for hello_ack -> CONNECTED + LISTENING */
    int waited = 0;
    while (!g_connected && waited < 8000) { goldie_msleep(50); waited += 50; }
    CHECK(g_connected, "hello_ack -> CONNECTED (session established)");
    CHECK(STATUS_SEEN(CONVAI_STATUS_LISTENING), "status LISTENING");

    /* uplink: 1.2s of 440Hz sine PCM16 8k mono in 20ms frames.
     * mock ASR emits is_final every 500ms -> triggers a full reply round. */
    int16_t pcm[160];
    convai_audio_frame_info_t ai = { .data_type = CONVAI_AUDIO_DATA_TYPE_PCM16 };
    for (int f = 0; f < 60; f++) {
        for (int i = 0; i < 160; i++) {
            pcm[i] = (int16_t)(8000.0 * sin(2.0 * 3.14159265 * 440.0 * (f * 160 + i) / 8000.0));
        }
        int rc = convai_send_audio(eng, pcm, sizeof(pcm), &ai);
        if (f == 0) CHECK(rc == CONVAI_OK, "convai_send_audio PCM16 accepted");
        goldie_msleep(20);
    }

    /* wait for the answer round */
    waited = 0;
    while (!STATUS_SEEN(CONVAI_STATUS_ANSWER_FINISHED) && waited < 15000) {
        goldie_msleep(100);
        waited += 100;
    }

    CHECK(STATUS_SEEN(CONVAI_STATUS_THINKING), "status THINKING (ASR is_final)");
    CHECK(g_text_msgs > 0, "LLM text reply received");
    CHECK(STATUS_SEEN(CONVAI_STATUS_ANSWERING), "status ANSWERING");
    CHECK(STATUS_SEEN(CONVAI_STATUS_ANSWER_FINISHED), "status ANSWER_FINISHED");
    printf("[e2e] downlink: %lu frames, %lu bytes PCM\n",
           g_audio_frames, g_audio_bytes);
    CHECK(g_audio_frames >= 30, "~40 TTS frames (0.8s @ 20ms) decoded");
    CHECK(g_audio_bytes >= 30 * 320, "PCM bytes match 20ms@8k frames");

    convai_open_mem_report();

    CHECK(convai_stop(eng) == CONVAI_OK, "convai_stop");
    convai_destroy(eng);
}

static void test_e2e(void)
{
    printf("-- e2e vs convai.v1 gateway (ws 明文 + wss TLS) --\n");
    /* 明文 ws 基线 */
    e2e_round("ws://127.0.0.1:19000/", NULL, 0, 0);
    /* WSS 全量校验: CA 验签 + CN/SAN 主机名校验 (SAN 含 DNS:localhost)。
     * 注: Go 网关内置偏好 AES-128-GCM, 默认宽名单下协商为 128;
     * AEAD (GCM) 是有保密+完整性的, 强制 256 见下轮。 */
    e2e_round("wss://localhost:19001/", "GCM", 0, 0);
    /* WSS IP 直连: CA 验签保留, 跳过 CN (mbedtls 对 IP SAN 字面匹配受限,
     * 设备按 IP 直连的生产形态, 见 convai_limits.h TLS 节) */
    e2e_round("wss://127.0.0.1:19001/", "GCM", 1, 0);
    /* WSS 强制 AES-256-GCM (收窄 offers, 网关只能选 256) */
    e2e_round("wss://localhost:19001/", "AES-256-GCM", 0, 1);
}

int main(void)
{
    printf("=== convai open SDK host E2E ===\n");
    test_ring();
    test_g711a();
    test_protocol();
    test_budget();
    test_e2e();

    printf("=== %s (%d failures) ===\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}

