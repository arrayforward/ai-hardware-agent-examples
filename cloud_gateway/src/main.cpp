/*!
 * @file main.cpp
 * @brief cloud_gateway entry point.
 */
#include "cloud_gateway/config.hpp"
#include "cloud_gateway/log.hpp"
#include "cloud_gateway/protocol.hpp"

#include "ws/server.hpp"
#include "session/session.hpp"
#include "session/session_manager.hpp"
#include "server/thread_pool.hpp"
#include "upstream/asr_backend.hpp"
#include "upstream/llm_backend.hpp"
#include "upstream/tts_backend.hpp"

#include <atomic>
#include <csignal>
#include <cstring>
#include <iostream>

namespace cg {

static std::atomic<bool> g_stop{false};

static void on_signal(int /*sig*/) { g_stop.store(true); }

} // namespace cg

int main(int argc, char** argv) {
    using namespace cg;
    Config cfg = Config::from_args(argc, argv);
    cfg.print();

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif

    ThreadPool pool(static_cast<std::size_t>(cfg.worker_threads));
    StubAsr    asr;
    StubLlm    llm;
    StubTts    tts;
    SessionManager sm;

    WsServer server(cfg.bind_address, cfg.port, &pool);

    server.set_on_connect([&](std::shared_ptr<WsConnection> conn) {
        auto sess = std::make_shared<Session>(conn, &sm, &asr, &llm, &tts, &pool);
        sm.add(sess);
        sess->bind();
        GATEWAY_LOG_INFO("[server] new connection accepted, session=%s",
                         sess->id().c_str());
    });

    if (!server.start()) {
        std::fprintf(stderr, "failed to start server\n");
        return 1;
    }

    GATEWAY_LOG_INFO("cloud_gateway up, threads=%zu", pool.size());

    std::thread watchdog([&]() {
        while (!g_stop.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        server.stop();
    });

    server.run();
    watchdog.join();

    GATEWAY_LOG_INFO("cloud_gateway shutting down (sessions=%zu)", sm.count());
    return 0;
}