/**
 * @file e2e_main.c
 * @brief Host-side E2E test for the WS63 open ConvAI SDK (aitalk/).
 *
 * Links the SAME sources as the WS63 firmware (aitalk/src + osal shim)
 * and runs scenarios against the live Go gateway + mock backends:
 *   1. convai_create/start -> hello -> hello_ack -> CONNECTED + LISTENING
 *   2. 30 x 20ms PCM16 tone uplink
 *   3. expect AI reply text + TTS audio frames + full status sequence
 * Scenarios: g711a and ima_adpcm.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "convai_open.h"
#include "convai_codec.h"

#define ROUTER_URL  "ws://127.0.0.1:9000/"
#define UPLINK_FRAMES 30

typedef struct {
    int connected, got_text, saw_thinking, saw_answering,
        saw_answer_finished, saw_listening;
    int audio_frames, audio_bytes;
    char reply[256];
} obs_t;

static obs_t g_obs;

static void on_event(convai_engine_t e, convai_event_t *ev, void *ud)
{
    (void)e; (void)ud;
    if (ev->code == CONVAI_EV_CONNECTED) g_obs.connected = 1;
    printf("[cb] event %d\n", ev->code);
}

static void on_status(convai_engine_t e, convai_status_e s, void *ud)
{
    (void)e; (void)ud;
    switch (s) {
    case CONVAI_STATUS_LISTENING:       g_obs.saw_listening = 1; break;
    case CONVAI_STATUS_THINKING:        g_obs.saw_thinking = 1; break;
    case CONVAI_STATUS_ANSWERING:       g_obs.saw_answering = 1; break;
    case CONVAI_STATUS_ANSWER_FINISHED: g_obs.saw_answer_finished = 1; break;
    default: break;
    }
    printf("[cb] status %d\n", s);
}

static void on_audio(convai_engine_t e, const void *data, size_t len,
                     const convai_audio_frame_info_t *info, void *ud)
{
    (void)e; (void)data; (void)info; (void)ud;
    g_obs.audio_frames++;
    g_obs.audio_bytes += (int)len;
}

static void on_message(convai_engine_t e, const void *data, size_t len,
                       const convai_message_info_t *info, void *ud)
{
    (void)e; (void)info; (void)ud;
    size_t n = len < sizeof(g_obs.reply) - 1 ? len : sizeof(g_obs.reply) - 1;
    memcpy(g_obs.reply, data, n);
    g_obs.reply[n] = 0;
    if (strstr(g_obs.reply, "\"text\"") || strstr(g_obs.reply, "小荷")) {
        g_obs.got_text = 1;
    }
    printf("[cb] message: %s\n", g_obs.reply);
}

static int wait_for(volatile int *cond, int timeout_ms)
{
    int waited = 0;
    while (!*cond && waited < timeout_ms) {
        usleep(100 * 1000);
        waited += 100;
    }
    return *cond;
}

static int run_scenario(int codec_id, const char *name)
{
    memset(&g_obs, 0, sizeof(g_obs));
    printf("\n=== scenario: %s ===\n", name);

    char cfg[768];
    snprintf(cfg, sizeof(cfg),
             "{\"info\":{\"product_id\":\"pid\",\"product_key\":\"pk\","
             "\"product_secret\":\"ps\",\"device_name\":\"e2e-ws63\"},"
             "\"ws\":{\"url\":\"%s\",\"audio\":{\"codec\":%d}}}",
             ROUTER_URL, codec_id);

    convai_event_handler_t h = {
        .on_convai_event = on_event,
        .on_convai_conversation_status = on_status,
        .on_convai_audio_data = on_audio,
        .on_convai_message_data = on_message,
    };
    convai_engine_t eng = NULL;
    int rc = convai_create(&eng, cfg, &h, NULL);
    if (rc != CONVAI_OK) {
        printf("[%s] FAIL create: %s\n", name, convai_err_2_str(rc));
        return 1;
    }
    convai_opt_t opt = { .mode = CONVAI_MODE_WS, .agent_id = "agent_e2e", .params = NULL };
    rc = convai_start(eng, &opt);
    if (rc != CONVAI_OK) {
        printf("[%s] FAIL start: %s\n", name, convai_err_2_str(rc));
        convai_destroy(eng);
        return 1;
    }

    int fails = 0;
    if (!wait_for(&g_obs.connected, 10000)) {
        printf("[%s] FAIL: no CONNECTED\n", name);
        fails++;
        goto out;
    }
    if (!g_obs.saw_listening) { printf("[%s] FAIL: no LISTENING\n", name); fails++; }

    {
        /* pace like a real mic (~10ms/frame) so the TX queue can drain */
        int sr = 8000, frame = sr / 50;
        int16_t *tone = malloc(frame * 2);
        for (int i = 0; i < frame; i++) {
            tone[i] = (int16_t)(10000 * ((i % 16) < 8 ? 1 : -1));
        }
        for (int f = 0; f < UPLINK_FRAMES; f++) {
            rc = convai_send_audio(eng, tone, frame * 2, NULL);
            if (rc != CONVAI_OK) break;
            usleep(10 * 1000);
        }
        free(tone);
        if (rc != CONVAI_OK) {
            printf("[%s] FAIL send_audio: %s\n", name, convai_err_2_str(rc));
            fails++;
            goto out;
        }
    }

    if (!wait_for(&g_obs.saw_answer_finished, 30000)) {
        printf("[%s] FAIL: turn not complete\n", name);
        fails++;
        goto out;
    }
    if (!g_obs.saw_thinking)  { printf("[%s] FAIL: no thinking\n", name); fails++; }
    if (!g_obs.saw_answering) { printf("[%s] FAIL: no answering\n", name); fails++; }
    if (!g_obs.got_text)      { printf("[%s] FAIL: no reply text\n", name); fails++; }
    if (g_obs.audio_frames < 5) {
        printf("[%s] FAIL: only %d audio frames\n", name, g_obs.audio_frames);
        fails++;
    }
    printf("[%s] stats: frames=%d bytes=%d\n", name, g_obs.audio_frames, g_obs.audio_bytes);

out:
    convai_open_mem_report();
    convai_stop(eng);
    convai_destroy(eng);
    printf("[%s] %s\n", name, fails ? "FAILED" : "PASSED");
    return fails;
}

int main(void)
{
    int fails = 0;
    /* wire codec ids (convai_codec.h): g711a=1, ima_adpcm=3 */
    fails += run_scenario(CONVAI_CODEC_G711A, "g711a");
    sleep(1);
    fails += run_scenario(CONVAI_CODEC_IMA_ADPCM, "ima_adpcm");

    printf("\n==========================================\n");
    printf("WS63-SDK E2E RESULT: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    printf("==========================================\n");
    return fails ? 1 : 0;
}
