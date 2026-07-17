/*!
 * @file log.hpp
 * @brief Tiny logging facade for cloud_gateway.
 *
 * Usage:
 *     GATEWAY_LOG_INFO("accepted fd=%d peer=%s", fd, peer);
 *     GATEWAY_LOG_WARN("session %s lost", sid);
 *     GATEWAY_LOG_ERROR("upstream failed");
 *
 * Levels: DEBUG < INFO < WARN < ERROR. Threshold controlled by env var
 * GATEWAY_LOG_LEVEL (one of: debug, info, warn, error). Default: info.
 */
#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

namespace cg {

enum class LogLevel : int {
    DEBUG = 0,
    INFO  = 1,
    WARN  = 2,
    LOG_ERROR = 3,
};

inline LogLevel parse_log_level(std::string_view s) {
    if (s == "debug" || s == "DEBUG") return LogLevel::DEBUG;
    if (s == "info"  || s == "INFO")  return LogLevel::INFO;
    if (s == "warn"  || s == "WARN")  return LogLevel::WARN;
    if (s == "error" || s == "ERROR") return LogLevel::LOG_ERROR;
    return LogLevel::INFO;
}

inline LogLevel current_log_level() {
    static LogLevel lvl = []() {
        const char* env = std::getenv("GATEWAY_LOG_LEVEL");
        return env ? parse_log_level(env) : LogLevel::INFO;
    }();
    return lvl;
}

inline const char* level_name(LogLevel l) {
    switch (l) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO ";
        case LogLevel::WARN:  return "WARN ";
        case LogLevel::LOG_ERROR: return "ERROR";
    }
    return "?";
}

inline std::mutex& log_mutex() {
    static std::mutex m;
    return m;
}

inline void log_write(LogLevel lvl, const char* file, int line,
                      const char* fmt, ...) {
    if (static_cast<int>(lvl) < static_cast<int>(current_log_level())) {
        return;
    }
    std::lock_guard<std::mutex> lk(log_mutex());
    char ts[32];
    auto now = std::time(nullptr);
    auto tm = *std::localtime(&now);
    std::strftime(ts, sizeof(ts), "%H:%M:%S", &tm);

    const char* base = std::strrchr(file, '/');
    base = base ? base + 1 : file;
#ifdef _WIN32
    const char* bs = std::strrchr(base, '\\');
    base = bs ? bs + 1 : base;
#endif

    std::fprintf(stderr, "%s %s [%s:%d] ", ts, level_name(lvl), base, line);

    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}

} // namespace cg

#if defined(GATEWAY_ENABLE_LOG)
#  define GATEWAY_LOG_DEBUG(...) ::cg::log_write(::cg::LogLevel::DEBUG,     __FILE__, __LINE__, __VA_ARGS__)
#  define GATEWAY_LOG_INFO(...)  ::cg::log_write(::cg::LogLevel::INFO,      __FILE__, __LINE__, __VA_ARGS__)
#  define GATEWAY_LOG_WARN(...)  ::cg::log_write(::cg::LogLevel::WARN,      __FILE__, __LINE__, __VA_ARGS__)
#  define GATEWAY_LOG_ERROR(...) ::cg::log_write(::cg::LogLevel::LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#else
#  define GATEWAY_LOG_DEBUG(...) ((void)0)
#  define GATEWAY_LOG_INFO(...)  ((void)0)
#  define GATEWAY_LOG_WARN(...)  ((void)0)
#  define GATEWAY_LOG_ERROR(...) ((void)0)
#endif