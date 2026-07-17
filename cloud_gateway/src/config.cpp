/*!
 * @file config.cpp
 * @brief Config::from_args implementation.
 */
#include "cloud_gateway/config.hpp"
#include "cloud_gateway/log.hpp"

#include <cstdlib>
#include <cstring>
#include <getopt.h>
#include <string>

namespace cg {

static void print_usage(const char* prog) {
    std::fprintf(stderr,
        "usage: %s [--bind ADDR] [--port N] [--threads N]\n"
        "         [--workers N] [--max-payload BYTES]\n"
        "         [--log-level debug|info|warn|error]\n"
        "         [--asr stub] [--llm stub] [--tts stub]\n"
        "\n"
        "  --bind ADDR        bind address (default 0.0.0.0)\n"
        "  --port N           listen port (default 9000)\n"
        "  --workers N        CPU worker thread count (default 4)\n"
        "  --max-payload N    max frame payload in bytes (default 65536)\n"
        "  --log-level LEVEL  log threshold (default info)\n"
        "  --asr|stub         ASR backend (only 'stub' supported)\n"
        "  --llm|stub         LLM backend (only 'stub' supported)\n"
        "  --tts|stub         TTS backend (only 'stub' supported)\n",
        prog);
}

Config Config::from_args(int argc, char** argv) {
    Config c;
    static struct option longopts[] = {
        {"bind",         required_argument, 0, 'b'},
        {"port",         required_argument, 0, 'p'},
        {"workers",      required_argument, 0, 'w'},
        {"max-payload",  required_argument, 0, 'm'},
        {"log-level",    required_argument, 0, 'l'},
        {"asr",          required_argument, 0,  1 },
        {"llm",          required_argument, 0,  2 },
        {"tts",          required_argument, 0,  3 },
        {"help",         no_argument,       0, 'h'},
        {0, 0, 0, 0},
    };
    int opt;
    int idx = 0;
    while ((opt = getopt_long(argc, argv, "b:p:w:m:l:h", longopts, &idx)) != -1) {
        switch (opt) {
            case 'b': c.bind_address = optarg; break;
            case 'p': c.port = static_cast<std::uint16_t>(std::atoi(optarg)); break;
            case 'w': c.worker_threads = std::atoi(optarg); break;
            case 'm': c.max_payload_size = static_cast<std::uint32_t>(std::atoi(optarg)); break;
            case 'l': {
#ifdef _WIN32
                _putenv_s("GATEWAY_LOG_LEVEL", optarg);
#else
                ::setenv("GATEWAY_LOG_LEVEL", optarg, 1);
#endif
                break;
            }
            case 1:  c.asr_backend = optarg; break;
            case 2:  c.llm_backend = optarg; break;
            case 3:  c.tts_backend = optarg; break;
            case 'h':
                print_usage(argv[0]);
                std::exit(0);
            default:
                print_usage(argv[0]);
                std::exit(2);
        }
    }
    return c;
}

void Config::print() const {
    std::fprintf(stderr,
        "[cloud_gateway config] bind=%s port=%u workers=%d "
        "max_payload=%u asr=%s llm=%s tts=%s\n",
        bind_address.c_str(), port, worker_threads, max_payload_size,
        asr_backend.c_str(), llm_backend.c_str(), tts_backend.c_str());
}

} // namespace cg