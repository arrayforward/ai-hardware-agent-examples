/**
 * @file convai_open.c
 * @brief Open-source ConvAI device SDK engine for WS63 (convai.v1).
 *
 * Replaces libconvai_sdk.a. Same public API (convai_api.h).
 * Platform deps: goldie_osal (threads/sem/mutex/malloc) + convai_ws_client.
 *
 * Threading model:
 *   - recv thread  : convai_wsc_poll -> text envelopes / binary audio frames
 *   - send thread  : drains the static TX queue -> ws binary frames
 *   - pump thread  : drains the RX message ring -> on_convai_audio_data
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "goldie_osal.h"
#include "cJSON.h"

#include "convai_open.h"
#include "convai_ws_client.h"
#include "convai_protocol.h"
#include "convai_codec.h"
#include "convai_ring.h"

#define TAG "convai_open"
#define LOGI(...) printf("[" TAG "] " __VA_ARGS__)
#define LOGE(...) printf("[" TAG "] ERROR: " __VA_ARGS__)

#define CONVAI_DEFAULT_URL "ws://192.168.1.100:9000/"
#define CONVAI_SUBPROTOCOL "convai.v1"

/* ---------------- static pools (no per-frame malloc) ---------------- */
static uint8_t  s_enc_buf[CONVAI_OPEN_ENC_BUF];
static int16_t  s_dec_pcm[CONVAI_OPEN_DEC_SAMPLES];
static uint8_t  s_rx_arena[CONVAI_OPEN_RX_RING_BYTES];

typedef struct {
    uint16_t len;
    uint8_t  data[CONVAI_OPEN_TX_SLOT];
} tx_frame_t;

typedef struct {
    /* config */
    char server_url[128];
    char product_id[64];
    char product_key[64];
    char product_secret[80];
    char device_name[64];
    char agent_id[80];
    char *startup_params;
    int  use_tls;            /* wss:// */
    int  tls_skip_cn;        /* IP 直连跳过 CN/SAN 校验 */
    int  tls_aes256_only;    /* 强制 AES-256-GCM (Go 网关默认偏好 128) */

    /* codec */
    const convai_codec_t *codec;
    convai_codec_state_t *codec_state;

    /* callbacks */
    convai_event_handler_t handler;
    void *user_data;

    /* transport */
    convai_wsc_t *ws;
    goldie_mutex send_mutex;     /* serializes ALL ws sends (tx buf is shared) */

    /* state */
    int started;
    int session_ready;
    int running;                 /* threads run flag */
    convai_status_e status;
    uint32_t tx_seq;
    uint32_t audio_seq;

    /* uplink queue */
    tx_frame_t tx_q[CONVAI_OPEN_TX_FRAMES];
    int tx_head, tx_count;
    goldie_mutex tx_mutex;
    goldie_sem tx_sem;
    uint32_t tx_drops, tx_high_water;
    void *send_thread;

    /* downlink ring */
    convai_ring_t rx_ring;
    goldie_mutex rx_mutex;
    goldie_sem rx_sem;
    void *pump_thread;

    void *recv_thread;
} engine_s;

static engine_s *g_eng;

/* ---------------- emit helpers ---------------- */

static void emit_event(engine_s *e, convai_event_code_e code, const char *details)
{
    if (e->handler.on_convai_event) {
        convai_event_t ev = { .code = code, .data.details = details };
        e->handler.on_convai_event(e, &ev, e->user_data);
    }
}

static void emit_status(engine_s *e, convai_status_e st)
{
    e->status = st;
    if (e->handler.on_convai_conversation_status) {
        e->handler.on_convai_conversation_status(e, st, e->user_data);
    }
}

static void emit_message(engine_s *e, const void *data, size_t len, int is_binary)
{
    if (e->handler.on_convai_message_data) {
        convai_message_info_t info = { .is_binary = is_binary };
        e->handler.on_convai_message_data(e, data, len, &info, e->user_data);
    }
}

static void emit_audio(engine_s *e, const void *data, size_t len)
{
    if (e->handler.on_convai_audio_data) {
        convai_audio_frame_info_t info = { .data_type = CONVAI_AUDIO_DATA_TYPE_PCM16 };
        e->handler.on_convai_audio_data(e, data, len, &info, e->user_data);
    }
}

/* ---------------- codec mgmt ---------------- */

static int set_codec_internal(engine_s *e, convai_codec_id_e id)
{
    const convai_codec_t *c = convai_codec_get(id);
    if (!c) {
        LOGE("codec %d not supported\n", id);
        return CONVAI_ERR_NOT_SUPPORTED;
    }
    if (e->codec && e->codec->deinit && e->codec_state) {
        e->codec->deinit(e->codec_state);
    }
    if (e->codec_state) { goldie_free(e->codec_state); e->codec_state = NULL; }
    if (c->state_size > 0) {
        e->codec_state = goldie_malloc(c->state_size);
        if (!e->codec_state) return CONVAI_ERR_OUT_OF_MEMORY;
        memset(e->codec_state, 0, c->state_size);
    }
    e->codec = c;
    if (c->init) c->init(e->codec_state);
    LOGI("codec -> %s (id=%d, sr=%d)\n", c->name, c->id, c->sample_rate);
    return CONVAI_OK;
}

int convai_open_set_codec(int codec_id)
{
    if (!g_eng || codec_id < 0 || codec_id >= CONVAI_CODEC_MAX) return CONVAI_ERR_INVALID_PARAM;
    return set_codec_internal(g_eng, (convai_codec_id_e)codec_id);
}

int convai_open_get_codec(void)
{
    return (g_eng && g_eng->codec) ? (int)g_eng->codec->id : -1;
}

const char *convai_open_get_ciphersuite(void)
{
    return (g_eng && g_eng->ws) ? convai_wsc_get_ciphersuite(g_eng->ws) : NULL;
}

/* ---------------- envelopes ---------------- */

static int send_envelope(engine_s *e, const char *type, const char *body_json)
{
    if (!convai_wsc_is_connected(e->ws)) return CONVAI_ERR_CONNECTION_LOST;
    char *out = convai_proto_build_envelope(type, body_json, ++e->tx_seq, 0 /*ts*/);
    if (!out) return CONVAI_ERR_OUT_OF_MEMORY;
    goldie_mutex_lock(&e->send_mutex);
    int rc = convai_wsc_send_text(e->ws, out, strlen(out));
    goldie_mutex_unlock(&e->send_mutex);
    free(out);
    return rc < 0 ? CONVAI_ERR_NETWORK : CONVAI_OK;
}

/* ---------------- uplink ---------------- */

static int tx_push(engine_s *e, const uint8_t *payload, size_t len)
{
    if (len > CONVAI_OPEN_TX_SLOT) return CONVAI_ERR_INVALID_PARAM;
    goldie_mutex_lock(&e->tx_mutex);
    if (e->tx_count >= CONVAI_OPEN_TX_FRAMES) {
        e->tx_head = (e->tx_head + 1) % CONVAI_OPEN_TX_FRAMES;   /* drop oldest */
        e->tx_count--;
        e->tx_drops++;
    }
    tx_frame_t *f = &e->tx_q[(e->tx_head + e->tx_count) % CONVAI_OPEN_TX_FRAMES];
    f->len = (uint16_t)len;
    memcpy(f->data, payload, len);
    e->tx_count++;
    if ((uint32_t)e->tx_count > e->tx_high_water) e->tx_high_water = e->tx_count;
    goldie_mutex_unlock(&e->tx_mutex);
    goldie_sem_post(&e->tx_sem);
    return CONVAI_OK;
}

static int send_task(void *arg)
{
    engine_s *e = arg;
    while (e->running) {
        goldie_sem_wait(&e->tx_sem);
        if (!e->running) break;
        goldie_mutex_lock(&e->tx_mutex);
        int have = e->tx_count > 0;
        tx_frame_t f;
        if (have) {
            f = e->tx_q[e->tx_head];
            e->tx_head = (e->tx_head + 1) % CONVAI_OPEN_TX_FRAMES;
            e->tx_count--;
        }
        goldie_mutex_unlock(&e->tx_mutex);
        if (!have || !convai_wsc_is_connected(e->ws)) continue;

        uint8_t hdr[CONVAI_AUDIO_HDR_LEN];
        convai_proto_audio_hdr_pack(hdr, CONVAI_AUDIO_OP_FRAME, ++e->audio_seq, 0);
        uint8_t frame[CONVAI_AUDIO_HDR_LEN + CONVAI_OPEN_TX_SLOT];
        memcpy(frame, hdr, CONVAI_AUDIO_HDR_LEN);
        memcpy(frame + CONVAI_AUDIO_HDR_LEN, f.data, f.len);
        goldie_mutex_lock(&e->send_mutex);
        convai_wsc_send_bin(e->ws, frame, CONVAI_AUDIO_HDR_LEN + f.len);
        goldie_mutex_unlock(&e->send_mutex);
    }
    return 0;
}

/* ---------------- downlink ---------------- */

static int pump_task(void *arg)
{
    engine_s *e = arg;
    static uint8_t msg[CONVAI_OPEN_DEC_SAMPLES * 2];
    uint16_t len;
    while (e->running) {
        goldie_sem_wait(&e->rx_sem);
        if (!e->running) break;
        goldie_mutex_lock(&e->rx_mutex);
        int rc = convai_ring_pop(&e->rx_ring, msg, sizeof(msg), &len);
        goldie_mutex_unlock(&e->rx_mutex);
        if (rc == 0) emit_audio(e, msg, len);
    }
    return 0;
}

/* ---------------- incoming frames ---------------- */

static void handle_text(engine_s *e, const char *data, size_t len)
{
    convai_envelope_t env;
    if (convai_proto_parse_envelope(data, len, &env) != 0) {
        LOGE("invalid text frame\n");
        return;
    }
    if (!strcmp(env.type, "hello_ack")) {
        const cJSON *ac = cJSON_GetObjectItemCaseSensitive(env.body, "audio_config");
        const cJSON *cn = ac ? cJSON_GetObjectItemCaseSensitive(ac, "codec") : NULL;
        if (cJSON_IsString(cn)) {
            const convai_codec_t *want = convai_codec_by_name(cn->valuestring);
            if (want && want != e->codec) set_codec_internal(e, want->id);
        }
        e->session_ready = 1;
        emit_event(e, CONVAI_EV_CONNECTED, "session established");
        emit_status(e, CONVAI_STATUS_LISTENING);
        if (e->startup_params && e->startup_params[0]) {
            send_envelope(e, "config_update", e->startup_params);
        }
    } else if (!strcmp(env.type, "hello_err")) {
        emit_event(e, CONVAI_EV_FAILED, "auth rejected");
    } else if (!strcmp(env.type, "status")) {
        const cJSON *st = cJSON_GetObjectItemCaseSensitive(env.body, "status");
        if (cJSON_IsString(st)) emit_status(e, convai_proto_status_from_str(st->valuestring));
    } else if (!strcmp(env.type, "event")) {
        const cJSON *ev = cJSON_GetObjectItemCaseSensitive(env.body, "event");
        const cJSON *dt = cJSON_GetObjectItemCaseSensitive(env.body, "details");
        const char *evs = cJSON_IsString(ev) ? ev->valuestring : "";
        const char *dts = cJSON_IsString(dt) ? dt->valuestring : NULL;
        if (!strcmp(evs, "connected"))         emit_event(e, CONVAI_EV_CONNECTED, dts);
        else if (!strcmp(evs, "disconnected")) emit_event(e, CONVAI_EV_DISCONNECTED, dts);
        else if (!strcmp(evs, "updated"))      emit_event(e, CONVAI_EV_UPDATED, dts);
        else                                   emit_event(e, CONVAI_EV_FAILED, dts);
    } else if (!strcmp(env.type, "pong")) {
        /* keepalive */
    } else {
        emit_message(e, data, len, 0);
    }
    convai_proto_envelope_free(&env);
}

static void handle_binary(engine_s *e, const uint8_t *data, size_t len)
{
    uint8_t op;
    if (convai_proto_audio_hdr_unpack(data, len, &op, NULL, NULL) != 0) return;
    switch (op) {
    case CONVAI_AUDIO_OP_FRAME: {
        if (!e->codec || !e->codec->decode) break;
        size_t samples = 0;
        if (e->codec->decode(e->codec_state, data + CONVAI_AUDIO_HDR_LEN,
                             len - CONVAI_AUDIO_HDR_LEN,
                             s_dec_pcm, CONVAI_OPEN_DEC_SAMPLES, &samples) == 0
            && samples > 0) {
            goldie_mutex_lock(&e->rx_mutex);
            convai_ring_push(&e->rx_ring, (const uint8_t *)s_dec_pcm,
                             (uint16_t)(samples * 2));
            goldie_mutex_unlock(&e->rx_mutex);
            goldie_sem_post(&e->rx_sem);
        }
        break;
    }
    case CONVAI_AUDIO_OP_START:
        emit_status(e, CONVAI_STATUS_ANSWERING);
        break;
    case CONVAI_AUDIO_OP_END:
        emit_status(e, CONVAI_STATUS_ANSWER_FINISHED);
        break;
    default:
        break;
    }
}

static void on_ws_frame(convai_wsc_event_e ev, const uint8_t *payload, size_t len, void *ud)
{
    engine_s *e = ud;
    switch (ev) {
    case CONVAI_WSC_EV_TEXT:   handle_text(e, (const char *)payload, len); break;
    case CONVAI_WSC_EV_BINARY: handle_binary(e, payload, len); break;
    case CONVAI_WSC_EV_CLOSED:
        e->session_ready = 0;
        emit_event(e, CONVAI_EV_DISCONNECTED, "transport closed");
        emit_status(e, CONVAI_STATUS_IDLE);
        break;
    }
}

static int recv_task(void *arg)
{
    engine_s *e = arg;
    while (e->running && convai_wsc_is_connected(e->ws)) {
        int rc = convai_wsc_poll(e->ws, on_ws_frame, e, 200);
        if (rc < 0) break;
    }
    if (e->running) {
        e->session_ready = 0;
        emit_event(e, CONVAI_EV_DISCONNECTED, "connection lost");
        emit_status(e, CONVAI_STATUS_IDLE);
    }
    return 0;
}

/* ---------------- URL parsing (ws|wss://host[:port][/path]) ---------------- */

static int parse_ws_url(const char *url, char *host, size_t host_cap,
                        uint16_t *port, char *path, size_t path_cap, int *is_tls)
{
    const char *p = url;
    *is_tls = 0;
    if (!strncmp(p, "ws://", 5))       { p += 5; *port = 9000; }
    else if (!strncmp(p, "wss://", 6)) { p += 6; *port = 443; *is_tls = 1; }
    else return -1;

    const char *slash = strchr(p, '/');
    const char *colon = slash ? NULL : strchr(p, ':');
    if (!colon && slash) {
        const char *c = memchr(p, ':', (size_t)(slash - p));
        colon = c;
    }
    size_t hlen = colon ? (size_t)(colon - p)
                        : (slash ? (size_t)(slash - p) : strlen(p));
    if (hlen == 0 || hlen >= host_cap) return -1;
    memcpy(host, p, hlen);
    host[hlen] = 0;
    if (colon) *port = (uint16_t)atoi(colon + 1);
    snprintf(path, path_cap, "%s", slash ? slash : "/");
    return 0;
}

/* ---------------- public API (same as convai_api.h) ---------------- */

int convai_create(convai_engine_t *handle,
                  const char *config_json,
                  const convai_event_handler_t *handler_fn,
                  void *user_data)
{
    if (!handle || !config_json || !handler_fn) return CONVAI_ERR_INVALID_PARAM;

    engine_s *e = goldie_malloc(sizeof(*e));
    if (!e) return CONVAI_ERR_OUT_OF_MEMORY;
    memset(e, 0, sizeof(*e));
    strncpy(e->server_url, CONVAI_DEFAULT_URL, sizeof(e->server_url) - 1);

    cJSON *root = cJSON_Parse(config_json);
    if (!root) { goldie_free(e); return CONVAI_ERR_INVALID_JSON; }
    const cJSON *info = cJSON_GetObjectItemCaseSensitive(root, "info");
    const cJSON *ws   = cJSON_GetObjectItemCaseSensitive(root, "ws");
    const cJSON *item;
#define COPY_STR(field, dst) \
    if (info && (item = cJSON_GetObjectItemCaseSensitive(info, field)) && cJSON_IsString(item)) \
        strncpy(e->dst, item->valuestring, sizeof(e->dst) - 1);
    COPY_STR("product_id", product_id);
    COPY_STR("product_key", product_key);
    COPY_STR("product_secret", product_secret);
    COPY_STR("device_name", device_name);
#undef COPY_STR
    int codec_id = CONVAI_CODEC_G711A;
    if (ws) {
        if ((item = cJSON_GetObjectItemCaseSensitive(ws, "url")) && cJSON_IsString(item)) {
            strncpy(e->server_url, item->valuestring, sizeof(e->server_url) - 1);
        }
        const cJSON *audio = cJSON_GetObjectItemCaseSensitive(ws, "audio");
        if (audio && (item = cJSON_GetObjectItemCaseSensitive(audio, "codec")) && cJSON_IsNumber(item)) {
            codec_id = item->valueint;
        }
        /* "tls":{"skip_cn_check":true} — IP 直连时跳过 CN/SAN 主机名校验
         * (CA 验签始终进行); "aes256_only":true — 强制 AES-256-GCM */
        const cJSON *tlsj = cJSON_GetObjectItemCaseSensitive(ws, "tls");
        if (tlsj && (item = cJSON_GetObjectItemCaseSensitive(tlsj, "skip_cn_check"))
                && cJSON_IsBool(item)) {
            e->tls_skip_cn = cJSON_IsTrue(item) ? 1 : 0;
        }
        if (tlsj && (item = cJSON_GetObjectItemCaseSensitive(tlsj, "aes256_only"))
                && cJSON_IsBool(item)) {
            e->tls_aes256_only = cJSON_IsTrue(item) ? 1 : 0;
        }
    }
    cJSON_Delete(root);

    if (!e->product_key[0] || !e->device_name[0]) {
        goldie_free(e);
        return CONVAI_ERR_CONFIG_INCOMPLETE;
    }

    memcpy(&e->handler, handler_fn, sizeof(e->handler));
    e->user_data = user_data;
    e->status = CONVAI_STATUS_IDLE;
    convai_ring_init(&e->rx_ring, s_rx_arena, sizeof(s_rx_arena));
    goldie_mutex_init(&e->send_mutex);
    goldie_mutex_init(&e->tx_mutex);
    goldie_mutex_init(&e->rx_mutex);
    goldie_sem_init(&e->tx_sem);
    goldie_sem_init(&e->rx_sem);

    if (set_codec_internal(e, (convai_codec_id_e)codec_id) != CONVAI_OK) {
        set_codec_internal(e, CONVAI_CODEC_G711A);
    }

    g_eng = e;
    *handle = e;
    LOGI("engine created (server=%s device=%s)\n", e->server_url, e->device_name);
    return CONVAI_OK;
}

void convai_destroy(convai_engine_t handle)
{
    engine_s *e = (engine_s *)handle;
    if (!e) return;
    if (e->started) convai_stop(handle);
    if (e->startup_params) goldie_free(e->startup_params);
    if (e->codec && e->codec->deinit && e->codec_state) e->codec->deinit(e->codec_state);
    if (e->codec_state) goldie_free(e->codec_state);
    if (g_eng == e) g_eng = NULL;
    goldie_free(e);
}

int convai_start(convai_engine_t handle, const convai_opt_t *opt)
{
    engine_s *e = (engine_s *)handle;
    if (!e) return CONVAI_ERR_NOT_INITIALIZED;
    if (e->started) return CONVAI_ERR_ALREADY_STARTED;

    if (opt && opt->agent_id) strncpy(e->agent_id, opt->agent_id, sizeof(e->agent_id) - 1);
    if (opt && opt->params) {
        if (e->startup_params) goldie_free(e->startup_params);
        e->startup_params = goldie_malloc(strlen(opt->params) + 1);
        if (e->startup_params) strcpy(e->startup_params, opt->params);
    }

    char host[64], path[32];
    uint16_t port;
    if (parse_ws_url(e->server_url, host, sizeof(host), &port, path, sizeof(path),
                     &e->use_tls) != 0) {
        LOGE("bad url %s\n", e->server_url);
        return CONVAI_ERR_INVALID_PARAM;
    }
    if (e->use_tls) {
        /* WSS: 内嵌 CA 验签 (TEST 证书见 convai_root_ca.c, 生产请替换) */
        extern const char g_convai_root_ca_pem[];
        convai_wsc_tls_t tls = {
            .ca_pem = g_convai_root_ca_pem,
            .skip_cn_check = e->tls_skip_cn,
            .ciphersuites = e->tls_aes256_only ? g_convai_wsc_suites_aes256 : NULL,
        };
        e->ws = convai_wsc_connect_ex(host, port, path, CONVAI_SUBPROTOCOL, &tls);
    } else {
        e->ws = convai_wsc_connect(host, port, path, CONVAI_SUBPROTOCOL);
    }
    if (!e->ws) {
        LOGE("ws%s connect failed %s:%u%s\n", e->use_tls ? "s" : "",
             host, port, path);
        return CONVAI_ERR_NETWORK;
    }
    if (e->use_tls) {
        LOGI("TLS established: %s (verify=CA%s)\n",
             convai_wsc_get_ciphersuite(e->ws),
             e->tls_skip_cn ? ", CN skipped" : "+CN");
    }

    e->running = 1;
    e->recv_thread = goldie_thread_create(recv_task, e, "convai_rx", CONVAI_OPEN_WS_TASK_STACK);
    e->pump_thread = goldie_thread_create(pump_task, e, "convai_pb", CONVAI_OPEN_PUMP_STACK);
    e->send_thread = goldie_thread_create(send_task, e, "convai_tx", CONVAI_OPEN_SEND_STACK);
    if (!e->recv_thread || !e->pump_thread || !e->send_thread) {
        e->running = 0;
        convai_wsc_destroy(e->ws);
        e->ws = NULL;
        return CONVAI_ERR_OUT_OF_MEMORY;
    }

    /* hello (protocol: first text message after upgrade) */
    char body[512];
    snprintf(body, sizeof(body),
             "{\"product_id\":\"%s\",\"product_key\":\"%s\","
             "\"product_secret\":\"%s\",\"device_name\":\"%s\","
             "\"audio_codec\":%d,\"sample_rate\":%d}",
             e->product_id, e->product_key, e->product_secret, e->device_name,
             e->codec ? e->codec->id : CONVAI_CODEC_G711A,
             e->codec ? e->codec->sample_rate : 8000);
    send_envelope(e, "hello", body);

    e->started = 1;
    LOGI("engine started (agent_id=%s)\n", e->agent_id);
    return CONVAI_OK;
}

int convai_stop(convai_engine_t handle)
{
    engine_s *e = (engine_s *)handle;
    if (!e) return CONVAI_ERR_NOT_INITIALIZED;
    if (!e->started) return CONVAI_OK;

    send_envelope(e, "bye", NULL);
    goldie_msleep(50);
    e->running = 0;
    goldie_sem_post(&e->tx_sem);
    goldie_sem_post(&e->rx_sem);

    /*
     * Teardown order matters (no use-after-free):
     *  1. delete the recv task first — it may be blocked inside
     *     convai_wsc_poll() on the socket;
     *  2. delete pump/send tasks (they only touch engine state);
     *  3. only then destroy the ws client (frees rx/tx buffers).
     */
    if (e->recv_thread)  { goldie_thread_destroy(e->recv_thread);  e->recv_thread = NULL; }
    if (e->pump_thread)  { goldie_thread_destroy(e->pump_thread);  e->pump_thread = NULL; }
    if (e->send_thread)  { goldie_thread_destroy(e->send_thread);  e->send_thread = NULL; }

    convai_wsc_destroy(e->ws);
    e->ws = NULL;
    e->started = 0;
    e->session_ready = 0;
    e->status = CONVAI_STATUS_IDLE;
    convai_ring_clear(&e->rx_ring);
    e->tx_head = e->tx_count = 0;
    LOGI("engine stopped\n");
    return CONVAI_OK;
}

int convai_update(convai_engine_t handle, const char *session_update_json)
{
    engine_s *e = (engine_s *)handle;
    if (!e) return CONVAI_ERR_NOT_INITIALIZED;
    if (!session_update_json) return CONVAI_ERR_INVALID_PARAM;
    return send_envelope(e, "config_update", session_update_json);
}

int convai_send_audio(convai_engine_t handle,
                      const void *data_ptr,
                      size_t data_len,
                      const convai_audio_frame_info_t *info_ptr)
{
    engine_s *e = (engine_s *)handle;
    if (!e) return CONVAI_ERR_NOT_INITIALIZED;
    if (!e->started || !e->session_ready) return CONVAI_ERR_SESSION_NOT_READY;
    if (!data_ptr || !data_len) return CONVAI_ERR_INVALID_PARAM;
    if (!e->codec || !e->codec->encode) return CONVAI_ERR_NOT_SUPPORTED;

    /* pre-encoded passthrough: caller already used the active codec.
     * Note: closed-SDK header has G711A=0 while open codec id G711A=1,
     * so accept both the codec id and the legacy G711A alias. */
    int passthrough = info_ptr && info_ptr->data_type != CONVAI_AUDIO_DATA_TYPE_PCM16
                      && ((int)info_ptr->data_type == (int)e->codec->id
                          || (e->codec->id == CONVAI_CODEC_G711A
                              && info_ptr->data_type == CONVAI_AUDIO_DATA_TYPE_G711A));
    if (passthrough) {
        return tx_push(e, data_ptr, data_len);
    }

    size_t frame_samples = (size_t)e->codec->sample_rate / 50;
    const int16_t *pcm = data_ptr;
    size_t total = data_len / 2;
    for (size_t off = 0; off < total; off += frame_samples) {
        size_t n = total - off;
        if (n > frame_samples) n = frame_samples;
        size_t enc_len = 0;
        if (e->codec->encode(e->codec_state, pcm + off, n,
                             s_enc_buf, sizeof(s_enc_buf), &enc_len) != 0 || enc_len == 0) {
            return CONVAI_ERR_MEDIA;
        }
        int rc = tx_push(e, s_enc_buf, enc_len);
        if (rc != CONVAI_OK) return rc;
    }
    return CONVAI_OK;
}

int convai_send_message(convai_engine_t handle,
                        const void *data_ptr,
                        size_t data_len,
                        const convai_message_info_t *info_ptr)
{
    engine_s *e = (engine_s *)handle;
    (void)info_ptr;
    if (!e) return CONVAI_ERR_NOT_INITIALIZED;
    if (!e->started || !e->session_ready) return CONVAI_ERR_SESSION_NOT_READY;
    if (!data_ptr || !data_len) return CONVAI_ERR_INVALID_PARAM;

    const char *s = data_ptr;
    if (s[0] == '{') {          /* complete envelope -> verbatim */
        goldie_mutex_lock(&e->send_mutex);
        int rc = convai_wsc_send_text(e->ws, s, data_len);
        goldie_mutex_unlock(&e->send_mutex);
        return rc < 0 ? CONVAI_ERR_NETWORK : CONVAI_OK;
    }
    cJSON *body = cJSON_CreateObject();
    if (!body) return CONVAI_ERR_OUT_OF_MEMORY;
    cJSON_AddStringToObject(body, "msg", s);
    char *bj = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!bj) return CONVAI_ERR_OUT_OF_MEMORY;
    int ret = send_envelope(e, "ping", bj);
    free(bj);
    return ret;
}

const char *convai_get_version(void) { return "0.2.0-open-ws63"; }

const char *convai_err_2_str(int err_code)
{
    switch (err_code) {
    case CONVAI_OK:                    return "Success";
    case CONVAI_ERR_INVALID_PARAM:     return "Invalid parameter";
    case CONVAI_ERR_OUT_OF_MEMORY:     return "Memory allocation failed";
    case CONVAI_ERR_NOT_INITIALIZED:   return "Engine not created";
    case CONVAI_ERR_ALREADY_STARTED:   return "Engine already started";
    case CONVAI_ERR_NOT_STARTED:       return "Engine not started";
    case CONVAI_ERR_NETWORK:           return "Network connection error";
    case CONVAI_ERR_TIMEOUT:           return "Operation timeout";
    case CONVAI_ERR_PROTOCOL:          return "Protocol error";
    case CONVAI_ERR_MEDIA:             return "Media error";
    case CONVAI_ERR_NOT_SUPPORTED:     return "Feature not supported";
    case CONVAI_ERR_CONNECTION_LOST:   return "Connection lost";
    case CONVAI_ERR_INIT_FAILED:       return "Engine initialization failed";
    case CONVAI_ERR_SESSION_NOT_READY: return "Session not ready";
    case CONVAI_ERR_CONFIG_INCOMPLETE: return "Config incomplete";
    case CONVAI_ERR_INVALID_JSON:      return "Invalid JSON";
    default:                           return "Unknown error";
    }
}

/* ---------------- mem report ---------------- */

/* Stub replacing the closed-SDK platform layer in open mode:
 * convai_bridge.c calls this; our engine needs no platform init. */
int convai_platform_ws63_init(void)
{
    return 0;
}

void convai_open_mem_report(void)
{
    engine_s *e = g_eng;
    if (!e) return;
    size_t codec_mem = (e->codec && e->codec->mem_usage && e->codec_state)
                       ? e->codec->mem_usage(e->codec_state) : 0;
    LOGI("=== convai_open mem report ===\n");
    LOGI("static pools: enc %dB dec %dB tx %dx%dB rx %dB\n",
         CONVAI_OPEN_ENC_BUF, CONVAI_OPEN_DEC_SAMPLES * 2,
         CONVAI_OPEN_TX_FRAMES, CONVAI_OPEN_TX_SLOT + 2, CONVAI_OPEN_RX_RING_BYTES);
    LOGI("codec: %s instance %uB\n", e->codec ? e->codec->name : "?", (unsigned)codec_mem);
    LOGI("tx queue: used %d/%d high-water %u drops %u\n",
         e->tx_count, CONVAI_OPEN_TX_FRAMES,
         (unsigned)e->tx_high_water, (unsigned)e->tx_drops);
    LOGI("rx ring: used %u/%dB high-water %uB drops %u\n",
         (unsigned)convai_ring_used(&e->rx_ring), CONVAI_OPEN_RX_RING_BYTES,
         (unsigned)convai_ring_high_water(&e->rx_ring),
         (unsigned)convai_ring_drops(&e->rx_ring));
}
