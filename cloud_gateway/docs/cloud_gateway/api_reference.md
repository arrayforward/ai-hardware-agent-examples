# API Reference

This document lists every public class and function in the gateway.
All identifiers live in the `cg` namespace.

---

## `cloud_gateway/log.hpp`

Tiny logging facade.

```cpp
namespace cg {
    enum class LogLevel { DEBUG, INFO, WARN, ERROR };
    LogLevel parse_log_level(std::string_view s);
    LogLevel current_log_level();              // respects GATEWAY_LOG_LEVEL env
    void     log_write(LogLevel, const char* file, int line, const char* fmt, ...);
}

#define GATEWAY_LOG_DEBUG(...)  // printf-style, INFO by default
#define GATEWAY_LOG_INFO(...)   // gated by LogLevel
#define GATEWAY_LOG_WARN(...)
#define GATEWAY_LOG_ERROR(...)
```

Compiled out if `GATEWAY_ENABLE_LOG` is undefined.

---

## `cloud_gateway/config.hpp` / `src/config.cpp`

```cpp
struct cg::Config {
    std::string   bind_address;       // default "0.0.0.0"
    std::uint16_t  port;               // default 9000
    int            listen_backlog;      // default 128
    int            io_threads;          // currently unused (always 1)
    int            worker_threads;      // default 4
    int            heartbeat_seconds;   // default 30 (currently 60s hard-coded)
    std::uint32_t  max_payload_size;    // default 64 KB
    int            session_ttl_seconds; // default 300 (currently unused)
    int            audio_buffer_ms;     // default 100 (currently unused)
    std::string   asr_backend;        // default "stub"
    std::string   llm_backend;        // default "stub"
    std::string   tts_backend;        // default "stub"
    bool          verbose;

    static Config from_args(int argc, char** argv);
    void print() const;
};
```

`Config::from_args` uses `getopt_long` to parse the CLI. See `cloud_gateway --help`.

---

## `cloud_gateway/protocol.hpp` / `src/protocol.cpp`

### Constants

```cpp
constexpr std::string_view WS_SUBPROTOCOL  = "convai.v1";
constexpr std::string_view WS_GUID         = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr std::size_t      AUDIO_HEADER    = 13;
constexpr std::size_t      AUDIO_MAX_BYTES = 32 * 1024;
```

### Close codes

```cpp
constexpr std::uint16_t CLOSE_NORMAL            = 1000;
constexpr std::uint16_t CLOSE_GOING_AWAY        = 1001;
constexpr std::uint16_t CLOSE_PROTOCOL_ERROR    = 1002;
constexpr std::uint16_t CLOSE_AUTH_FAILED       = 4401;
constexpr std::uint16_t CLOSE_SESSION_NOT_FOUND = 4404;
constexpr std::uint16_t CLOSE_RATE_LIMITED      = 4429;
constexpr std::uint16_t CLOSE_INTERNAL_ERROR    = 1011;
```

### Enums

```cpp
enum class MsgType : std::uint16_t {
    Hello, HelloAck, HelloErr, Bye, Ping, Pong,
    Status, Event, Text, TextDelta,
    ConfigUpdate, ConfigUpdateAck, ConfigUpdateErr,
    FunctionCall, FunctionCallOutput,
    Error, Ack,
};

enum class AudioOp : std::uint8_t { Frame=0x10, Start=0x11, End=0x12, Cancel=0x13 };

enum class AgentStatus : std::uint8_t {
    Idle, Listening, Thinking, Answering, Interrupted, AnswerFinished,
};

enum class SessionEventKind : std::uint8_t {
    Connected, Disconnected, Failed, Updated,
};
```

### Structures

```cpp
struct Envelope {
    MsgType                  type;
    std::uint32_t            seq;
    std::uint64_t            ts_ms;
    std::string              body_json;   // raw JSON
};

struct AudioFrame {
    AudioOp                 op;
    std::uint32_t           seq;
    std::uint64_t           ts_ms;
    std::vector<std::uint8_t> pcm;
};
```

### Conversion helpers

```cpp
const char*      msg_type_name(MsgType);
std::string_view msg_type_str(MsgType) noexcept;
bool             parse_msg_type(std::string_view, MsgType&) noexcept;
const char*      agent_status_name(AgentStatus);
const char*      session_event_name(SessionEventKind);
```

### Byte-order helpers

```cpp
std::uint16_t hton16/std::uint16_t ntoh16(...);
std::uint32_t hton32/std::uint32_t ntoh32(...);
std::uint64_t hton64/std::uint64_t ntoh64(...);
```

---

## `cloud_gateway/session.hpp`

```cpp
namespace cg {
    inline std::int64_t  now_ms() noexcept;        // steady clock
    inline std::uint64_t unix_ms() noexcept;       // wall clock
    using SessionId = std::string;
}
```

---

## `src/util/json_helper.hpp`

Self-contained JSON encoder/decoder (subset).

```cpp
struct JsonNode {
    JsonValue v;     // variant<null,bool,int64,double,string,vector,map>

    bool is_null()   / is_bool()   / is_number() / is_string()
        / is_array() / is_object();
    bool                as_bool();
    std::int64_t        as_int();
    double              as_double();
    const std::string&  as_string();
    const std::vector<JsonNode>& as_array();
    std::vector<JsonNode>&       as_array();
    const std::map<std::string,JsonNode>& as_object();
    std::map<std::string,JsonNode>&       as_object();
    const JsonNode* find(const std::string& key) const;
};

class JsonWriter {
public:
    std::string str() const;
    void write_null / write_bool / write_number / write_string / write_value(...);
};

std::string to_json(const JsonNode& n);
JsonNode    parse_json(std::string_view text);
```

Throws `ParseError` on malformed input.

---

## `src/util/base64.hpp` / `sha1.hpp`

```cpp
std::string cg::base64_encode(std::string_view data);

class cg::Sha1 {
public:
    Sha1();
    void update(const std::uint8_t* data, std::size_t len);
    void finalize(std::uint8_t out[20]);
};
std::string sha1_hex(const std::string& data);
```

---

## `src/ws/frame.hpp` / `src/ws/frame.cpp`

RFC 6455 frame parser (server side, no fragmentation, single-frame only).

```cpp
enum class WsOpcode : std::uint8_t {
    Continuation=0, Text=1, Binary=2, Close=8, Ping=9, Pong=0xA,
};

struct WsFrame {
    bool         fin = true;
    WsOpcode     op;
    std::vector<std::uint8_t> payload;
};

enum class ParseStatus { NeedMore, Ok, Error };

class WsFrameParser {
public:
    std::size_t push(const std::uint8_t* data, std::size_t len);
    bool        has_frame() const;
    WsFrame     pop();
    void        reset();
    bool        has_error() const;
    const std::string& error() const;
    void        set_max_payload(std::size_t);
};

// Serializers (server side never masks outgoing).
std::vector<std::uint8_t> ws_serialize(WsOpcode, const std::uint8_t*, std::size_t);
std::vector<std::uint8_t> ws_serialize(WsOpcode, std::string_view);
std::vector<std::uint8_t> ws_close_frame(std::uint16_t code, std::string_view reason);
std::vector<std::uint8_t> ws_pong_frame(const std::uint8_t*, std::size_t);
```

---

## `src/ws/handshake.hpp` / `src/ws/handshake.cpp`

```cpp
struct HandshakeRequest {
    std::string method, path, version;
    std::string sec_key, sec_protocol, host;
};

struct ParseResult {
    enum Status { NeedMore, Ok, Error };
    Status          status;
    std::string     error;
    HandshakeRequest req;
    std::size_t     consumed;
};

ParseResult  parse_handshake(std::string_view buf);
std::string  build_handshake_response(const HandshakeRequest&);
```

---

## `src/ws/server.hpp` / `src/ws/server.cpp`

```cpp
class WsConnection : public std::enable_shared_from_this<WsConnection> {
public:
    enum class State { Handshaking, Open, Closing, Closed };
    WsConnection(int fd, const sockaddr_in& peer, WsServer*);

    int                       fd()    const;
    State                     state() const;
    const sockaddr_in&        peer()  const;
    std::string               peer_str() const;

    void on_text  (std::function<void(WsConnection*, std::string)>);
    void on_binary(std::function<void(WsConnection*, const std::uint8_t*, std::size_t)>);
    void on_close (std::function<void(WsConnection*, std::uint16_t, std::string)>);

    void send_text  (std::string_view);
    void send_binary(const std::uint8_t*, std::size_t);
    void send_binary(std::string_view);
    void close      (std::uint16_t code, std::string_view reason);
    void schedule_close();
};

class WsServer {
public:
    WsServer(std::string bind_addr, std::uint16_t port, ThreadPool* pool);
    bool start();
    void stop();
    void run();   // blocking
    void set_on_connect(std::function<void(std::shared_ptr<WsConnection>)>);

    int listen_fd() const;
    ThreadPool* pool() const;
};
```

The server owns `std::unordered_map<int, std::shared_ptr<WsConnection>>`
and removes connections on `CLOSE_NORMAL` or after `close()` from the
caller.

---

## `src/codec/envelope.hpp` / `src/codec/envelope.cpp`

```cpp
class EnvelopeCodec {
public:
    static std::string encode_text(const Envelope&);                  // -> JSON
    static bool        decode_text(const std::string&, Envelope&, std::string& err);
    static std::string encode(MsgType, uint32_t seq, uint64_t ts, const JsonNode& body);
    static std::vector<std::uint8_t> encode_audio(const AudioFrame&);
    static bool        decode_audio(const std::uint8_t*, std::size_t, AudioFrame&, std::string& err);
};
```

---

## `src/server/thread_pool.hpp` / `src/server/thread_pool.cpp`

```cpp
class ThreadPool {
public:
    explicit ThreadPool(std::size_t n_threads);
    ~ThreadPool();

    template <typename F, typename... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>;

    std::size_t size() const noexcept;
};
```

Tasks are dispatched FIFO; workers block on a condition variable
when the queue is empty.

---

## `src/session/session.hpp` / `src/session/session.cpp`

```cpp
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(std::shared_ptr<WsConnection>, SessionManager*,
            AsrBackend*, LlmBackend*, TtsBackend*, ThreadPool*);

    const SessionId& id() const;
    void bind();           // attach to WsConnection callbacks
    void detach();         // release the connection reference

    // Inbound handlers (called by Session itself when it sees frames)
    void on_hello(const JsonNode&);
    void on_bye();
    void on_config_update(const JsonNode&);
    void on_function_call_output(const JsonNode&);
    void on_audio_binary(const AudioFrame&);
    void on_close(uint16_t code, std::string reason);
};
```

A session is created per accepted connection (see
`SessionManager::add`). All inbound `convai_*` messages are translated
into this class.

---

## `src/session/session_manager.hpp` / `src/session/session_manager.cpp`

```cpp
class SessionManager {
public:
    void                                   add(std::shared_ptr<Session>);
    void                                   remove(const SessionId&);
    std::shared_ptr<Session>               get(const SessionId&);
    std::size_t                            count() const;
};
```

Thread-safe (uses a single mutex).

---

## Upstream backends

All three share the same pattern: an interface + a stub implementation
that produces deterministic output for testing.

### `AsrBackend`

```cpp
class AsrBackend {
public:
    using Callback = std::function<void(std::string text, bool is_final)>;
    virtual void feed(const std::uint8_t* g711a, std::size_t n_bytes,
                      int sample_rate, Callback cb) = 0;
};
class StubAsr : public AsrBackend { ... };
```

`StubAsr::feed` computes a simple "activity ratio" over the A-law
bytes. When the ratio crosses 0.30 it emits a fixed transcript.

### `LlmBackend`

```cpp
struct LlmChunk {
    std::string kind;     // "text" or "function_call"
    std::string content;
};

class LlmBackend {
public:
    using Callback = std::function<void(std::vector<LlmChunk> batch)>;
    virtual void chat(const std::string& asr_text,
                      const JsonNode& session_config,
                      Callback cb) = 0;
    virtual void feed_tool_result(const std::string& call_id,
                                  const std::string& output,
                                  Callback cb) = 0;
};
```

`StubLlm` emits a fixed script: 3 text fragments + 1 function call
(`emotion=happy`) + 1 more text. This produces an end-to-end loop
including the device-side `function_call_output` reply.

### `TtsBackend`

```cpp
class TtsBackend {
public:
    using Callback = std::function<void(std::vector<AudioFrame> batch)>;
    virtual void synth(const std::string& text, const std::string& voice_type,
                       Callback cb) = 0;
};
```

`StubTts` synthesizes a 440 Hz sine wave encoded as G.711A, with an
amplitude envelope so that consecutive frames sound like a smoothly
modulated tone. The number of frames scales with text length so longer
inputs produce longer audio.

### Plugging in real backends

Implement the interface, register the impl in `main.cpp`:

```cpp
class MyAsr : public cg::AsrBackend { ... };
// in main:
MyAsr    asr;     // instead of StubAsr
```

The interface is intentionally narrow so a real-world implementation
(iFlytek / Whisper / Azure) is a thin wrapper.